#include "rnd_validation_driver.h"

#include <snk/driver/rnd_pipeline_cache.h>
#include <snk/common/engine_assert.h>

#include <algorithm>
#include <array>
#include <format>
#include <optional>

#include <snk/driver/rnd_bind_group.h>

namespace {
	using namespace rnd::driver;

	bool is_depth_or_stencil_format(texture_header::TYPE format)
	{
		switch (format) {
		case texture_header::TYPE::D32F_S8:
		case texture_header::TYPE::D24_S8:
		case texture_header::TYPE::D32:
		case texture_header::TYPE::D24:
		case texture_header::TYPE::D16:
		case texture_header::TYPE::D32F:
		case texture_header::TYPE::S1:
		case texture_header::TYPE::S4:
		case texture_header::TYPE::S8:
			return true;
		default:
			return false;
		}
	}

	std::uint32_t mip_extent(std::uint32_t size, std::uint32_t mip)
	{
		return std::max<std::uint32_t>(size >> std::min<std::uint32_t>(mip, 31), 1u);
	}

	// Slices a render pass can select with `attachment_desc::layer`.
	std::uint32_t layer_count(const texture_interface& texture)
	{
		switch (texture.get_type()) {
		case TEXTURE_TYPE::TEXTURE_3D: return std::max(texture.depth(), 1u);
		case TEXTURE_TYPE::TEXTURE_CUBE_MAP: return 6;
		case TEXTURE_TYPE::TEXTURE_1D_ARRAY:
		case TEXTURE_TYPE::TEXTURE_2D_ARRAY: return std::max(texture.layers(), 1u);
		case TEXTURE_TYPE::TEXTURE_CUBE_ARRAY: return std::max(texture.layers(), 1u) * 6;
		default: return 1;
		}
	}

	const char* format_name(texture_format format)
	{
		static constexpr const char* names[] = { "R8", "RGB8", "RGBA8", "RGBA12", "RGBA16", "RGBA16F", "RGBA32F", "R32I", "R32F",
			"D32F_S8", "D24_S8", "D32", "D24", "D16", "D32F", "S1", "S4", "S8" };
		return names[static_cast<std::size_t>(format)];
	}

	std::string formats_text(const ds::fixed_vector<std::optional<texture_format>, pipeline_desc::max_targets>& colors, const std::optional<texture_format>& depth)
	{
		std::string text = "colors [";
		for (std::size_t i = 0; i < colors.size(); ++i) {
			text += std::format("{}{}", i ? ", " : "", colors[i] ? format_name(*colors[i]) : "none");
		}
		text += std::format("], depth {}", depth ? format_name(*depth) : "none");
		return text;
	}

	const char* pass_label(const render_pass_desc& desc)
	{
		return desc.debug_name ? desc.debug_name : "unnamed";
	}
}

rnd::driver::validation::driver::driver(std::unique_ptr<driver_interface> inner, limits budget)
	: driver(std::move(inner), std::make_shared<validator>(budget))
{
}

rnd::driver::validation::driver::driver(std::unique_ptr<driver_interface> inner, std::shared_ptr<validator> state)
	: m_inner(std::move(inner))
	, m_inner_textures(dynamic_cast<const texture_object_access*>(m_inner.get()))
	, m_state(std::move(state))
{
	// Not an assert: it must hold in release builds too. Without the access create_texture fails closed.
	ENGINE_VERIFY(m_inner_textures, "the validation driver wraps a driver that gives no access to its texture objects (texture_object_access); it cannot make textures");
}

bool rnd::driver::validation::driver::check_render_pass(const render_pass_desc& desc, render_pass_desc& inner_desc, target_info& target)
{
	const std::string label = pass_label(desc);
	const auto invalid = [&](const std::string& why) {
		m_state->report(rule::invalid_render_pass, std::format("render pass '{}': {}", label, why));
	};

	bool alive = true;
	for (const attachment_desc& color : desc.colors) {
		alive = (!color.texture || live_texture(color.texture, "color attachment")) && alive;
	}
	if (desc.depth_stencil) {
		alive = (!desc.depth_stencil->texture || live_texture(desc.depth_stencil->texture, "depth attachment")) && alive;
	}
	if (!alive) {
		return false;
	}

	if (desc.backbuffer) {
		bool has_texture = desc.depth_stencil && desc.depth_stencil->texture;
		for (const attachment_desc& color : desc.colors) {
			has_texture = has_texture || color.texture;
		}
		if (has_texture) {
			invalid("a backbuffer pass takes no texture attachments");
		}
	} else {
		if (desc.colors.empty() && !desc.depth_stencil) {
			invalid("no attachments and not a backbuffer pass");
		}
		if (desc.depth_stencil && !desc.depth_stencil->texture) {
			invalid("depth/stencil attachment without a texture");
		}
	}

	std::optional<glm::uvec2> extent;
	bool size_mismatch = false;
	const auto visit = [&](const attachment_desc& attachment, bool is_depth) {
		const texture_interface& texture = *m_textures.find(attachment.texture);
		const bool usage_ok = is_depth
			? texture.has_usage(texture_usage::depth_target) || texture.has_usage(texture_usage::stencil_target)
			: texture.has_usage(texture_usage::color_target);
		if (!usage_ok) {
			m_state->report(rule::attachment_usage, std::format("render pass '{}': {} attachment of {}x{} has usage {}, expected {}",
				label, is_depth ? "depth" : "color", texture.width(), texture.height(), validation::usage_text(texture.get_usage()),
				is_depth ? "depth_target or stencil_target" : "color_target"));
		}
		if (attachment.mip >= std::max(texture.mip_levels(), 1u)) {
			invalid(std::format("mip {} of a texture with {} mip levels", attachment.mip, texture.mip_levels()));
		}
		if (attachment.layer != attachment_desc::all_layers && attachment.layer >= layer_count(texture)) {
			invalid(std::format("layer {} of a texture with {} layers", attachment.layer, layer_count(texture)));
		}
		const glm::uvec2 size{ mip_extent(texture.width(), attachment.mip), mip_extent(texture.height(), attachment.mip) };
		if (!extent) {
			extent = size;
		} else if (*extent != size) {
			size_mismatch = true;
		}
	};
	for (const attachment_desc& color : desc.colors) {
		if (color.texture) {
			visit(color, false);
		}
	}
	if (desc.depth_stencil && desc.depth_stencil->texture) {
		visit(*desc.depth_stencil, true);
	}
	if (size_mismatch) {
		m_state->report(rule::attachment_size_mismatch, std::format("render pass '{}': attachments differ in size", label));
	}

	target.has_extent = extent.has_value();
	target.width = extent ? extent->x : 0;
	target.height = extent ? extent->y : 0;

	const glm::ivec4 area = desc.render_area;
	if (area.z > 0 && area.w > 0 && target.has_extent) {
		const bool inside = area.x >= 0 && area.y >= 0
			&& static_cast<std::int64_t>(area.x) + area.z <= target.width
			&& static_cast<std::int64_t>(area.y) + area.w <= target.height;
		if (!inside) {
			m_state->report(rule::render_area_outside_target, std::format("render pass '{}': render area {{{}, {}, {}, {}}} is outside the {}x{} target",
				label, area.x, area.y, area.z, area.w, target.width, target.height));
		}
	}

	inner_desc = desc;
	for (attachment_desc& color : inner_desc.colors) {
		color.texture = inner_handle(color.texture);
	}
	if (inner_desc.depth_stencil) {
		inner_desc.depth_stencil->texture = inner_handle(inner_desc.depth_stencil->texture);
	}
	return true;
}

void rnd::driver::validation::driver::begin_render_pass(const render_pass_desc& desc)
{
	if (m_state->in_pass()) {
		m_state->report(rule::nested_render_pass, std::format("begin_render_pass('{}') inside the pass '{}'", pass_label(desc), m_state->pass_name()));
		m_state->drop_begin();
		return;
	}

	render_pass_desc inner_desc;
	target_info target;
	if (!check_render_pass(desc, inner_desc, target)) {
		m_state->drop_begin();
		return;
	}
	pipeline_desc formats;
	set_target_formats(*this, formats, desc);
	m_state->begin_pass(pass_label(desc), target, { formats.color_formats, formats.depth_format },
		static_cast<std::uint32_t>(desc.colors.size()), desc.depth_stencil.has_value());
	m_bound_sets = {};
	m_bound_pipeline = 0;
	m_inner->begin_render_pass(inner_desc);
}

void rnd::driver::validation::driver::end_render_pass()
{
	if (m_state->consume_dropped_begin()) {
		return;
	}
	if (!m_state->end_pass()) {
		m_state->report(rule::end_without_begin, "end_render_pass outside a render pass");
		return;
	}
	m_inner->end_render_pass();
}

void rnd::driver::validation::driver::set_viewport(glm::ivec4 rect)
{
	m_state->note(trace_op::set_viewport);
	const target_info& target = m_state->current_target();
	const bool inside = !target.has_extent || (rect.x >= 0 && rect.y >= 0 && rect.z >= 0 && rect.w >= 0
		&& static_cast<std::int64_t>(rect.x) + rect.z <= target.width
		&& static_cast<std::int64_t>(rect.y) + rect.w <= target.height);
	if (!inside) {
		m_state->report(rule::viewport_outside_target, std::format("viewport {{{}, {}, {}, {}}} is outside the {}x{} render target",
			rect.x, rect.y, rect.z, rect.w, target.width, target.height));
	}
	m_inner->set_viewport(rect);
}

const rnd::driver::validation::vertex_array* rnd::driver::validation::driver::check_draw(
	const vertex_array_interface* va, unsigned int count, std::size_t base_index)
{
	if (!m_state->require_live(va, object_kind::vertex_array, "draw vertex array")) {
		return nullptr;
	}
	const auto* wrapped = static_cast<const vertex_array*>(va);
	m_state->note(trace_op::draw, count);
	m_state->on_draw();
	if (!m_state->in_pass()) {
		m_state->report(rule::draw_outside_render_pass, "draw outside a render pass");
	}
	if (!m_state->pipeline_bound()) {
		m_state->report(rule::draw_without_pipeline, "draw before a pipeline is bound in this render pass");
	} else if (m_bound_pipeline != 0) {
		check_pipeline_sets(m_bound_pipeline);
	}
	if (!wrapped->has_layout()) {
		m_state->report(rule::draw_without_layout, "draw with a vertex array that has no vertex buffer layout");
	}
	if (count + base_index > wrapped->index_capacity()) {
		m_state->report(rule::draw_index_range, std::format("draw of {} indices from index {} exceeds the index buffer of {} indices",
			count, base_index, wrapped->index_capacity()));
	}
	return wrapped;
}

void rnd::driver::validation::driver::draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, unsigned int count, unsigned int offset)
{
	if (const vertex_array* wrapped = check_draw(vertices.get(), count, 0)) {
		m_inner->draw_indices(wrapped->inner(), count, offset);
	}
}

void rnd::driver::validation::driver::draw_indices(const vertex_array_interface* va, unsigned int count, unsigned int base_vertex, unsigned int base_index)
{
	if (const vertex_array* wrapped = check_draw(va, count, base_index)) {
		m_inner->draw_indices(wrapped->inner().get(), count, base_vertex, base_index);
	}
}

bool rnd::driver::validation::driver::shader_alive(const shader_ref& ref)
{
	if (!m_state->require_live(ref.object, object_kind::shader, "pipeline shader")) {
		return false;
	}
	// A live shader at the address is not the one the pipeline was made for if the pipeline's shader died.
	if (ref.object->serial() != ref.serial) {
		m_state->report(rule::dead_object, std::format("pipeline shader {} was deleted, another shader lives at its address", static_cast<const void*>(ref.object)));
		return false;
	}
	return true;
}

rnd::driver::pipeline_handle rnd::driver::validation::driver::create_pipeline(const pipeline_desc& desc)
{
	if (!desc.shader) {
		m_state->report(rule::invalid_pipeline, "pipeline without a shader");
		return {};
	}
	if (!shader_alive(desc.shader)) {
		return {};
	}
	if (desc.blend.size() > desc.color_formats.size()) {
		m_state->report(rule::invalid_pipeline, std::format("pipeline blends {} targets but declares {} color formats",
			desc.blend.size(), desc.color_formats.size()));
	}

	for (const bind_group_layout_handle& layout : desc.set_layouts) {
		if (layout && !m_layouts.contains(key_of(layout))) {
			m_state->report(rule::invalid_pipeline, std::format("pipeline names a bind group layout that is not one of this driver's (index {}, generation {})", layout.index, layout.gen));
			return {};
		}
	}

	pipeline_desc inner_desc = desc;
	inner_desc.shader = &static_cast<const shader*>(desc.shader.object)->inner();
	const pipeline_handle handle = m_inner->create_pipeline(inner_desc);
	if (handle.index == m_pipelines.size() + 1) {
		m_pipelines.push_back({ handle, desc });
		m_state->on_pipeline_created();
	}
	return handle;
}

void rnd::driver::validation::driver::bind_pipeline(pipeline_handle pipeline)
{
	m_state->note(trace_op::bind_pipeline, pipeline.index);
	if (!m_state->in_pass()) {
		m_state->report(rule::bind_pipeline_outside_pass, "bind_pipeline outside a render pass");
		return;
	}
	const bool known = pipeline.index != 0 && pipeline.index <= m_pipelines.size() && m_pipelines[pipeline.index - 1].handle == pipeline;
	if (!known) {
		m_state->report(rule::invalid_pipeline, std::format("bind_pipeline with a handle of no pipeline (index {}, generation {})", pipeline.index, pipeline.gen));
		return;
	}
	const pipeline_desc& desc = m_pipelines[pipeline.index - 1].desc;
	if (!shader_alive(desc.shader)) {
		return;
	}

	const pass_formats& expected = m_state->current_formats();
	if (!(desc.color_formats == expected.colors) || desc.depth_format != expected.depth) {
		m_state->report(rule::pipeline_format_mismatch, std::format("pipeline {} declares {}, render pass '{}' renders to {}",
			pipeline.index, formats_text(desc.color_formats, desc.depth_format), m_state->pass_name(), formats_text(expected.colors, expected.depth)));
	}
	m_state->on_pipeline_bound();
	m_bound_pipeline = pipeline.index;
	m_inner->bind_pipeline(pipeline);
}

const rnd::driver::pipeline_desc& rnd::driver::validation::driver::pipeline_info(pipeline_handle pipeline) const
{
	const bool known = pipeline.index != 0 && pipeline.index <= m_pipelines.size() && m_pipelines[pipeline.index - 1].handle == pipeline;
	return known ? m_pipelines[pipeline.index - 1].desc : empty_pipeline_desc();
}

std::unique_ptr<rnd::driver::shader_interface> rnd::driver::validation::driver::create_shader(const std::vector<shader_header>& headers)
{
	auto created = m_inner->create_shader(headers);
	if (!created) {
		return nullptr;
	}
	m_state->on_program_created();
	return std::make_unique<shader>(m_state, std::move(created));
}

void rnd::driver::validation::driver::check_texture_usage(const texture_header& header)
{
	const std::string what = std::format("{}x{} texture '{}'", header.data.extent.width, header.data.extent.height, header.name);
	if (header.usage == texture_usage::none) {
		m_state->report(rule::texture_usage_flags, what + " has no usage flags");
		return;
	}
	if (is_depth_or_stencil_format(header.data.format)) {
		if (!has_any_usage(header.usage, texture_usage::depth_target | texture_usage::stencil_target)) {
			m_state->report(rule::texture_usage_flags, std::format("{} has a depth/stencil format but usage {}", what, usage_text(header.usage)));
		}
		if (has_usage(header.usage, texture_usage::color_target)) {
			m_state->report(rule::texture_usage_flags, what + " has a depth/stencil format and the color_target usage");
		}
	}
	if (header.data.initial_data && !has_usage(header.usage, texture_usage::sampled)) {
		m_state->report(rule::texture_usage_flags, std::format("{} has initial data but usage {}, expected sampled", what, usage_text(header.usage)));
	}
}

rnd::driver::texture_handle rnd::driver::validation::driver::create_texture(const texture_header& header)
{
	check_texture_usage(header);
	if (!m_inner_textures) {
		return {};
	}
	const texture_handle created = m_inner->create_texture(header);
	texture_interface* const inner_texture = m_inner_textures->texture_object(created);
	if (!inner_texture) {
		return {};
	}
	m_state->on_texture_created();
	return m_textures.adopt(std::make_unique<texture>(m_state, *inner_texture, created, header));
}

void rnd::driver::validation::driver::destroy(texture_handle handle)
{
	if (!handle || !live_texture(handle, "destroy")) {
		return;
	}
	std::unique_ptr<texture_interface> wrapper = m_textures.release(handle);
	const texture_handle inner = static_cast<texture&>(*wrapper).handle_in_inner();
	wrapper.reset();
	m_inner->destroy(inner);
}

rnd::driver::validation::texture* rnd::driver::validation::driver::live_texture(texture_handle handle, const char* what)
{
	if (texture_interface* found = m_textures.find(handle)) {
		return static_cast<texture*>(found);
	}
	if (m_textures.stale(handle)) {
		m_state->report(rule::stale_handle, std::format("{} names a texture that was destroyed (handle index {}, generation {})", what, handle.index, handle.gen));
	} else {
		m_state->report(rule::dead_object, std::format("{} (handle index {}, generation {}) is not a texture of this driver", what, handle.index, handle.gen));
	}
	return nullptr;
}

void rnd::driver::validation::driver::update_texture(texture_handle handle, const void* data, const texture_region& region)
{
	validation::texture* const found = live_texture(handle, "update_texture");
	if (!found) {
		return;
	}
	found->update_data(data, region.x, region.y, region.z, region.width, region.height, region.depth, region.level, region.layer, region.face);
}

rnd::driver::texture_handle rnd::driver::validation::driver::inner_handle(texture_handle handle) const
{
	const texture_interface* found = m_textures.find(handle);
	return found ? static_cast<const texture*>(found)->handle_in_inner() : texture_handle{};
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::driver::validation::driver::create_vertex_array()
{
	return std::make_unique<vertex_array>(m_state, m_inner->create_vertex_array());
}

std::unique_ptr<rnd::driver::buffer_interface> rnd::driver::validation::driver::create_buffer()
{
	return std::make_unique<buffer>(m_state, std::shared_ptr<buffer_interface>(m_inner->create_buffer()));
}

std::vector<std::byte> rnd::driver::validation::driver::read_texture(texture_handle handle, glm::ivec4 rect)
{
	const texture_interface* found = live_texture(handle, "read_texture");
	if (!found) {
		return {};
	}
	const texture_interface& texture = *found;
	if (!can_read_back(texture)) {
		m_state->report(rule::invalid_read, std::format("read_texture of a {}x{} texture that is not a 2D color texture", texture.width(), texture.height()));
		return {};
	}
	if (!read_rect_inside(texture, rect)) {
		m_state->report(rule::invalid_read, std::format("read_texture of rect {}, {}, {}x{} outside a {}x{} texture",
			rect.x, rect.y, rect.z, rect.w, texture.width(), texture.height()));
		return {};
	}
	return m_inner->read_texture(inner_handle(handle), rect);
}

void rnd::driver::validation::driver::begin_frame()
{
	m_state->on_begin_frame();
	const std::uint64_t epoch = m_state->frame_epoch();
	for (auto it = m_groups.begin(); it != m_groups.end();) {
		if (it->second.life == lifetime::transient) {
			m_expired_groups.push_back({ it->first, epoch });
			it = m_groups.erase(it);
		} else {
			++it;
		}
	}
	constexpr std::uint64_t remembered_frames = 8;
	std::erase_if(m_expired_groups, [&](const expired_group& e) { return e.epoch + remembered_frames < epoch; });
	m_inner->begin_frame();
}

rnd::driver::buffer_handle rnd::driver::validation::driver::create_buffer(const buffer_desc& desc)
{
	if (desc.size == 0) {
		m_state->report(rule::invalid_buffer, "create_buffer of size 0");
		return {};
	}
	const buffer_handle handle = m_inner->create_buffer(desc);
	if (handle) {
		m_buffers[key_of(handle)] = desc;
	}
	return handle;
}

void rnd::driver::validation::driver::destroy_buffer(buffer_handle buffer)
{
	if (m_buffers.erase(key_of(buffer)) == 0) {
		m_state->report(rule::invalid_buffer, std::format("destroy_buffer with a handle of no buffer (index {}, generation {})", buffer.index, buffer.gen));
		return;
	}
	m_inner->destroy_buffer(buffer);
}

void rnd::driver::validation::driver::update_buffer(buffer_handle buffer, std::size_t offset, std::span<const std::byte> data)
{
	const auto it = m_buffers.find(key_of(buffer));
	if (it == m_buffers.end()) {
		m_state->report(rule::invalid_buffer, std::format("update_buffer with a handle of no buffer (index {}, generation {})", buffer.index, buffer.gen));
		return;
	}
	const std::size_t capacity = it->second.size;
	if (offset > capacity || data.size() > capacity - offset) {
		m_state->report(rule::buffer_write_range, std::format("update_buffer of {} bytes at offset {} exceeds the buffer size {}", data.size(), offset, capacity));
		return;
	}
	m_inner->update_buffer(buffer, offset, data);
}

rnd::driver::bind_group_layout_handle rnd::driver::validation::driver::create_bind_group_layout(const bind_group_layout_desc& desc)
{
	bool valid = true;
	const auto reject = [&](rule kind, const std::string& why) {
		m_state->report(kind, "bind group layout: " + why);
		valid = false;
	};
	const binding_table& table = bindings();
	for (std::size_t i = 0; i < desc.entries.size(); ++i) {
		const binding_decl& decl = desc.entries[i];
		const std::uint32_t slots_anywhere = std::max({ table.slots(0, decl.type), table.slots(1, decl.type), table.slots(2, decl.type), table.slots(3, decl.type) });
		if (decl.slot >= slots_anywhere) {
			reject(rule::binding_outside_table, std::format("slot {} of type {} exists in no set of the binding table (at most {})",
				decl.slot, static_cast<int>(decl.type), slots_anywhere));
		}
		if (decl.dynamic && decl.type != binding_type::ubo) {
			reject(rule::bind_group_layout_mismatch, std::format("slot {} is dynamic but not a uniform buffer", decl.slot));
		}
		if (static_cast<std::uint8_t>(decl.stages) == 0) {
			reject(rule::bind_group_layout_mismatch, std::format("slot {} is visible to no shader stage", decl.slot));
		}
		for (std::size_t j = 0; j < i; ++j) {
			if (desc.entries[j].type == decl.type && desc.entries[j].slot == decl.slot) {
				reject(rule::bind_group_layout_mismatch, std::format("slot {} of type {} is declared twice", decl.slot, static_cast<int>(decl.type)));
			}
		}
	}
	if (!valid) {
		return {};
	}
	const bind_group_layout_handle handle = m_inner->create_bind_group_layout(desc);
	if (handle) {
		m_layouts[key_of(handle)] = desc;
	}
	return handle;
}

bool rnd::driver::validation::driver::check_buffer_entry(const binding_res& entry, const binding_decl& decl)
{
	const auto buffer = m_buffers.find(key_of(entry.buffer));
	if (buffer == m_buffers.end()) {
		m_state->report(rule::invalid_buffer, std::format("bind group entry at slot {} names a buffer that is not live (index {}, generation {})",
			entry.slot, entry.buffer.index, entry.buffer.gen));
		return false;
	}
	const buffer_usage wanted = decl.type == binding_type::ubo ? buffer_usage::uniform : buffer_usage::storage;
	bool valid = true;
	if (buffer->second.usage != wanted) {
		m_state->report(rule::bind_group_layout_mismatch, std::format("bind group entry at slot {} is a {} binding but the buffer is not a {} buffer",
			entry.slot, decl.type == binding_type::ubo ? "uniform" : "storage", decl.type == binding_type::ubo ? "uniform" : "storage"));
		valid = false;
	}
	if (entry.size == 0 || entry.offset > buffer->second.size || entry.size > buffer->second.size - entry.offset) {
		m_state->report(rule::invalid_bind_group, std::format("bind group entry at slot {} covers {} bytes from offset {} of a buffer of {} bytes",
			entry.slot, entry.size, entry.offset, buffer->second.size));
		valid = false;
	}
	return valid;
}

rnd::driver::bind_group_handle rnd::driver::validation::driver::create_bind_group(const bind_group_desc& desc, lifetime life)
{
	const auto layout = m_layouts.find(key_of(desc.layout));
	if (layout == m_layouts.end()) {
		m_state->report(rule::invalid_bind_group, std::format("create_bind_group with a layout that is not one of this driver's (index {}, generation {})",
			desc.layout.index, desc.layout.gen));
		return {};
	}

	bool valid = true;
	if (desc.entries.size() != layout->second.entries.size()) {
		m_state->report(rule::bind_group_layout_mismatch, std::format("bind group has {} entries, its layout has {}", desc.entries.size(), layout->second.entries.size()));
		valid = false;
	}
	bind_group_desc inner_desc = desc;
	for (const binding_decl& decl : layout->second.entries) {
		const auto found = std::find_if(desc.entries.begin(), desc.entries.end(),
			[&](const binding_res& e) { return e.type == decl.type && e.slot == decl.slot; });
		if (found == desc.entries.end()) {
			m_state->report(rule::bind_group_layout_mismatch, std::format("bind group has no entry for slot {} of type {}", decl.slot, static_cast<int>(decl.type)));
			valid = false;
			continue;
		}
		if (decl.type == binding_type::texture) {
			if (!found->texture) {
				m_state->report(rule::bind_group_layout_mismatch, std::format("bind group entry at texture slot {} has no texture", decl.slot));
				valid = false;
			} else if (const texture_interface* bound = live_texture(found->texture, "bind group texture"); !bound) {
				valid = false;
			} else if (!bound->has_usage(texture_usage::sampled)) {
				m_state->report(rule::texture_not_sampled, std::format("{}x{} texture with usage {} in a bind group for sampling",
					bound->width(), bound->height(), usage_text(bound->get_usage())));
				valid = false;
			}
		} else {
			valid = check_buffer_entry(*found, decl) && valid;
		}
	}
	for (const binding_res& entry : desc.entries) {
		const bool declared = std::any_of(layout->second.entries.begin(), layout->second.entries.end(),
			[&](const binding_decl& d) { return d.type == entry.type && d.slot == entry.slot; });
		if (!declared) {
			m_state->report(rule::bind_group_layout_mismatch, std::format("bind group entry at slot {} of type {} is not in its layout", entry.slot, static_cast<int>(entry.type)));
			valid = false;
		}
	}
	if (!valid) {
		return {};
	}

	for (binding_res& entry : inner_desc.entries) {
		entry.texture = inner_handle(entry.texture);
	}
	const bind_group_handle handle = m_inner->create_bind_group(inner_desc, life);
	if (handle) {
		m_groups[key_of(handle)] = { desc.layout, desc, life, m_state->frame_epoch() };
		m_state->on_bind_group_created(life == lifetime::transient);
	}
	return handle;
}

void rnd::driver::validation::driver::destroy_bind_group(bind_group_handle group)
{
	if (m_groups.erase(key_of(group)) == 0) {
		m_state->report(rule::invalid_bind_group, std::format("destroy_bind_group with a handle of no group (index {}, generation {})", group.index, group.gen));
		return;
	}
	m_inner->destroy_bind_group(group);
}

void rnd::driver::validation::driver::set_bind_group(std::uint32_t set, bind_group_handle group, std::span<const std::uint32_t> dynamic_offsets)
{
	m_state->note(trace_op::bind_group, set, group.index);
	if (!m_state->in_pass()) {
		m_state->report(rule::bind_group_outside_pass, std::format("set_bind_group({}) outside a render pass", set));
		return;
	}
	if (set >= max_bind_sets) {
		m_state->report(rule::binding_outside_table, std::format("set_bind_group at set {}, the binding table has {} sets", set, max_bind_sets));
		return;
	}

	const auto found = m_groups.find(key_of(group));
	if (found == m_groups.end()) {
		const bool expired = std::any_of(m_expired_groups.begin(), m_expired_groups.end(), [&](const expired_group& e) { return e.key == key_of(group); });
		if (expired) {
			m_state->report(rule::transient_group_expired, std::format("transient bind group {} was made in an earlier frame (now frame {}) and is gone", group.index, m_state->frame_epoch()));
		} else {
			m_state->report(rule::invalid_bind_group, std::format("set_bind_group with a handle of no group (index {}, generation {})", group.index, group.gen));
		}
		return;
	}
	const known_group& known = found->second;
	if (known.life == lifetime::transient && known.epoch != m_state->frame_epoch()) {
		m_state->report(rule::transient_group_expired, std::format("transient bind group {} was made in frame {}, used in frame {}", group.index, known.epoch, m_state->frame_epoch()));
		return;
	}

	const bind_group_layout_desc& layout = m_layouts.at(key_of(known.layout));
	const binding_table& table = bindings();
	bool valid = true;
	for (const binding_decl& decl : layout.entries) {
		if (!table.contains(set, decl.slot, decl.type)) {
			m_state->report(rule::binding_outside_table, std::format("slot {} of type {} is outside the binding table at set {} (it has {})",
				decl.slot, static_cast<int>(decl.type), set, table.slots(set, decl.type)));
			valid = false;
		}
	}

	const std::size_t dynamic_count = dynamic_entry_count(layout);
	if (dynamic_offsets.size() != dynamic_count) {
		m_state->report(rule::invalid_bind_group, std::format("set_bind_group({}) gives {} dynamic offsets, the layout has {} dynamic bindings", set, dynamic_offsets.size(), dynamic_count));
		valid = false;
	}
	const std::size_t alignment = uniform_offset_alignment();
	const std::size_t storage_alignment = storage_offset_alignment();
	std::size_t next_dynamic = 0;
	for (const binding_decl& decl : layout.entries) {
		const auto entry = std::find_if(known.desc.entries.begin(), known.desc.entries.end(),
			[&](const binding_res& e) { return e.type == decl.type && e.slot == decl.slot; });
		if (entry == known.desc.entries.end()) {
			continue;
		}
		if (decl.type == binding_type::texture) {
			valid = (live_texture(entry->texture, "bound group texture") != nullptr) && valid;
			continue;
		}
		const auto buffer = m_buffers.find(key_of(entry->buffer));
		if (buffer == m_buffers.end()) {
			m_state->report(rule::invalid_buffer, std::format("bind group {} names a buffer that was destroyed (slot {})", group.index, entry->slot));
			valid = false;
			continue;
		}
		// The offset the group itself gives (a dynamic offset is added to it, and checked below).
		const std::size_t static_alignment = decl.type == binding_type::ubo ? alignment : storage_alignment;
		if (entry->offset % static_alignment != 0) {
			m_state->report(rule::binding_offset_alignment, std::format("offset {} of the {} entry at slot {} is not a multiple of the {} offset alignment {}",
				entry->offset, decl.type == binding_type::ubo ? "uniform" : "storage", decl.slot, decl.type == binding_type::ubo ? "uniform" : "storage", static_alignment));
			valid = false;
		}
		if (!decl.dynamic || next_dynamic >= dynamic_offsets.size()) {
			continue;
		}
		const std::size_t extra = dynamic_offsets[next_dynamic++];
		if (extra % alignment != 0) {
			m_state->report(rule::dynamic_offset_alignment, std::format("dynamic offset {} at slot {} is not a multiple of the uniform offset alignment {}", extra, decl.slot, alignment));
			valid = false;
		} else if (entry->offset + extra + entry->size > buffer->second.size) {
			m_state->report(rule::invalid_bind_group, std::format("dynamic offset {} moves the {} byte range at slot {} past the end of the {} byte buffer",
				extra, entry->size, decl.slot, buffer->second.size));
			valid = false;
		}
	}
	if (!valid) {
		return;
	}
	m_bound_sets[set] = group;
	m_inner->set_bind_group(set, group, dynamic_offsets);
}

void rnd::driver::validation::driver::check_pipeline_sets(std::size_t pipeline_index)
{
	const pipeline_desc& desc = m_pipelines[pipeline_index - 1].desc;
	for (std::uint32_t set = 0; set < desc.set_layouts.size(); ++set) {
		const bind_group_layout_handle needed = desc.set_layouts[set];
		if (!needed) {
			continue;
		}
		const bind_group_handle bound = m_bound_sets[set];
		if (!bound) {
			m_state->report(rule::draw_missing_bind_group, std::format("draw with pipeline {} needs a bind group at set {}, none is bound in render pass '{}'",
				pipeline_index, set, m_state->pass_name()));
			continue;
		}
		const auto group = m_groups.find(key_of(bound));
		if (group == m_groups.end()) {
			m_state->report(rule::invalid_bind_group, std::format("draw with pipeline {}: the bind group at set {} was destroyed after it was bound", pipeline_index, set));
		} else if (group->second.layout != needed) {
			m_state->report(rule::bind_group_layout_mismatch, std::format("draw with pipeline {}: the bind group at set {} has layout {}, the pipeline expects layout {}",
				pipeline_index, set, group->second.layout.index, needed.index));
		}
	}
}

rnd::driver::validation::swapchain::swapchain(std::unique_ptr<swapchain_interface> inner, limits budget)
	: m_inner(std::move(inner))
	, m_state(std::make_shared<validator>(budget))
{
}

std::unique_ptr<rnd::driver::driver_interface> rnd::driver::validation::swapchain::create_driver()
{
	return std::make_unique<driver>(m_inner->create_driver(), m_state);
}

void rnd::driver::validation::swapchain::present()
{
	m_state->on_present();
	m_inner->present();
}
