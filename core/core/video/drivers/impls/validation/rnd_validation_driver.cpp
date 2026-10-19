#include "rnd_validation_driver.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>

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

	texture_interface* unwrap(texture_interface* wrapped)
	{
		return wrapped ? &static_cast<validation::texture*>(wrapped)->inner() : nullptr;
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
	, m_state(std::move(state))
{
}

bool rnd::driver::validation::driver::check_render_pass(const render_pass_desc& desc, render_pass_desc& inner_desc, target_info& target)
{
	const std::string label = pass_label(desc);
	const auto invalid = [&](const std::string& why) {
		m_state->report(rule::invalid_render_pass, std::format("render pass '{}': {}", label, why));
	};

	bool alive = true;
	for (const attachment_desc& color : desc.colors) {
		alive = (!color.texture || m_state->require_live(color.texture, object_kind::texture, "color attachment")) && alive;
	}
	if (desc.depth_stencil) {
		alive = (!desc.depth_stencil->texture || m_state->require_live(desc.depth_stencil->texture, object_kind::texture, "depth attachment")) && alive;
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
		const texture_interface& texture = *attachment.texture;
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
		if (attachment.layer >= layer_count(texture)) {
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
		color.texture = unwrap(color.texture);
	}
	if (inner_desc.depth_stencil) {
		inner_desc.depth_stencil->texture = unwrap(inner_desc.depth_stencil->texture);
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
	m_state->begin_pass(pass_label(desc), target, static_cast<std::uint32_t>(desc.colors.size()), desc.depth_stencil.has_value());
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
	if (!m_state->program_used()) {
		m_state->report(rule::draw_without_use, "draw before any shader use() in this frame");
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

void rnd::driver::validation::driver::draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int offset)
{
	if (const vertex_array* wrapped = check_draw(vertices.get(), count, 0)) {
		m_inner->draw_indices(wrapped->inner(), render_mode, count, offset);
	}
}

void rnd::driver::validation::driver::draw_indices(const vertex_array_interface* va, RENDER_MODE rm, unsigned int count, unsigned int base_vertex, unsigned int base_index)
{
	if (const vertex_array* wrapped = check_draw(va, count, base_index)) {
		m_inner->draw_indices(wrapped->inner().get(), rm, count, base_vertex, base_index);
	}
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

std::unique_ptr<rnd::driver::texture_interface> rnd::driver::validation::driver::create_texture(const texture_header& header)
{
	check_texture_usage(header);
	auto created = m_inner->create_texture(header);
	if (!created) {
		return nullptr;
	}
	m_state->on_texture_created();
	return std::make_unique<texture>(m_state, std::move(created), header);
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::driver::validation::driver::create_vertex_array()
{
	return std::make_unique<vertex_array>(m_state, m_inner->create_vertex_array());
}

std::unique_ptr<rnd::driver::buffer_interface> rnd::driver::validation::driver::create_buffer()
{
	return std::make_unique<buffer>(m_state, std::shared_ptr<buffer_interface>(m_inner->create_buffer()));
}

std::unique_ptr<rnd::driver::uniform_buffer_interface> rnd::driver::validation::driver::create_uniform_buffer(std::size_t size, std::size_t binding)
{
	return std::make_unique<uniform_buffer>(m_state, m_inner->create_uniform_buffer(size, binding), size);
}

std::unique_ptr<rnd::driver::ssbo_buffer_interface> rnd::driver::validation::driver::create_ssbo_buffer(std::size_t size, std::size_t binding)
{
	return std::make_unique<ssbo_buffer>(m_state, m_inner->create_ssbo_buffer(size, binding), size);
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
