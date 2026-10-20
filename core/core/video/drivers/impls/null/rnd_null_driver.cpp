#include "rnd_null_driver.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

#include <cstring>

rnd::driver::null::driver::driver()
	: m_stats(std::make_shared<object_stats>())
{
}

void rnd::driver::null::driver::begin_render_pass(const render_pass_desc& desc)
{
	ASSERT_MSG(!m_in_render_pass, "begin_render_pass inside a render pass");
	if (!desc.backbuffer) {
		const auto key = make_framebuffer_key(desc, [this](texture_handle handle) -> std::uint64_t {
			const texture* found = texture_of(handle);
			return found ? found->framebuffer_object() : 0;
		});
		m_stats->framebuffers.acquire(key, [this] { return ++m_next_framebuffer; });
	}
	m_last_pass = desc;
	m_in_render_pass = true;
	for (bound_set& set : m_bound_sets) {
		set = {};
	}
	++m_calls.render_pass_begins;
}

void rnd::driver::null::driver::end_render_pass()
{
	ASSERT_MSG(m_in_render_pass, "end_render_pass outside a render pass");
	m_in_render_pass = false;
	++m_calls.render_pass_ends;
}

void rnd::driver::null::driver::count_draw()
{
	++m_calls.draws;
	++m_draws_by_topology[static_cast<std::size_t>(pipeline_info(m_bound).topology)];
	for (std::size_t set = 0; set < max_bind_sets; ++set) {
		if (m_bound_sets[set].group) {
			++m_draws_with_set[set];
		}
	}
}

void rnd::driver::null::driver::draw_indices(const std::unique_ptr<vertex_array_interface>& /*vertices*/, unsigned int /*count*/, unsigned int /*offset*/)
{
	count_draw();
}

void rnd::driver::null::driver::draw_indices(const vertex_array_interface* /*va*/, unsigned int /*count*/, unsigned int /*base_vertex*/, unsigned int /*base_index*/)
{
	count_draw();
}

std::unique_ptr<rnd::driver::shader_interface> rnd::driver::null::driver::create_shader(const std::vector<shader_header>& headers)
{
	const std::string error = validate_shader_headers(headers);
	if (!error.empty()) {
		egLOG_ERROR("shader/load", "null driver rejected the shader: {}", error);
		return nullptr;
	}
	if (m_record_shader_sources) {
		m_shader_sources.push_back(headers);
	}
	return std::make_unique<shader>(m_stats, headers);
}

rnd::driver::texture_handle rnd::driver::null::driver::create_texture(const texture_header& header)
{
	return m_textures.adopt(std::make_unique<texture>(m_stats, header));
}

void rnd::driver::null::driver::destroy(texture_handle texture)
{
	if (texture) {
		m_textures.release(texture);
	}
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::driver::null::driver::create_vertex_array()
{
	return std::make_unique<vertex_array>(m_stats);
}

std::unique_ptr<rnd::driver::buffer_interface> rnd::driver::null::driver::create_buffer()
{
	return std::make_unique<buffer>(m_stats);
}

std::vector<std::byte> rnd::driver::null::driver::read_texture(texture_handle handle, glm::ivec4 rect)
{
	const texture* found = texture_of(handle);
	if (!ENGINE_VERIFY(found, "null driver: read_texture with a handle of no texture (index {}, generation {})", handle.index, handle.gen)) {
		return {};
	}
	if (!can_read_back(*found) || !read_rect_inside(*found, rect)) {
		return {};
	}
	std::vector<std::byte> bytes;
	bytes.reserve(static_cast<std::size_t>(rect.z) * static_cast<std::size_t>(rect.w) * 4);
	for (int y = 0; y < rect.w; ++y) {
		for (int x = 0; x < rect.z; ++x) {
			bytes.push_back(static_cast<std::byte>((rect.x + x) & 0xff));
			bytes.push_back(static_cast<std::byte>((rect.y + y) & 0xff));
			bytes.push_back(std::byte{ 0 });
			bytes.push_back(std::byte{ 0xff });
		}
	}
	return bytes;
}

void rnd::driver::null::driver::begin_frame()
{
	++m_frame;
	++m_calls.frames_begun;
	for (const bind_group_handle group : m_transient_groups) {
		m_groups.erase(group);
	}
	m_transient_groups.clear();
}

rnd::driver::buffer_handle rnd::driver::null::driver::create_buffer(const buffer_desc& desc)
{
	null_buffer buffer{ desc, {} };
	const std::size_t copies = desc.update == buffer_update::per_frame ? frames_in_flight : 1;
	buffer.copies.assign(copies, std::vector<std::byte>(desc.size));
	++m_calls.buffers_created;
	return m_buffers.insert(std::move(buffer));
}

void rnd::driver::null::driver::destroy_buffer(buffer_handle buffer)
{
	ENGINE_VERIFY(m_buffers.erase(buffer), "null driver: destroy_buffer with a handle of no buffer (index {}, generation {})", buffer.index, buffer.gen);
}

void rnd::driver::null::driver::update_buffer(buffer_handle buffer, std::size_t offset, std::span<const std::byte> data)
{
	null_buffer* target = m_buffers.find(buffer);
	if (!ENGINE_VERIFY(target, "null driver: update_buffer with a handle of no buffer (index {}, generation {})", buffer.index, buffer.gen)) {
		return;
	}
	std::vector<std::byte>& bytes = target->copies[target->desc.update == buffer_update::per_frame ? frame_slot() : 0];
	if (!ENGINE_VERIFY(offset <= bytes.size() && data.size() <= bytes.size() - offset,
		"null driver: update of {} bytes at offset {} exceeds the buffer size {}", data.size(), offset, bytes.size())) {
		return;
	}
	if (!data.empty()) {
		std::memcpy(bytes.data() + offset, data.data(), data.size());
	}
	++m_calls.buffer_updates;
}

rnd::driver::bind_group_layout_handle rnd::driver::null::driver::create_bind_group_layout(const bind_group_layout_desc& desc)
{
	return m_layouts.acquire(desc);
}

rnd::driver::bind_group_handle rnd::driver::null::driver::create_bind_group(const bind_group_desc& desc, lifetime life)
{
	ASSERT_MSG(m_layouts.find(desc.layout), "create_bind_group with a handle of no layout");
	const bind_group_handle group = m_groups.insert({ desc, life });
	if (life == lifetime::transient) {
		m_transient_groups.push_back(group);
		++m_calls.transient_bind_groups_created;
	} else {
		++m_calls.bind_groups_created;
	}
	return group;
}

void rnd::driver::null::driver::destroy_bind_group(bind_group_handle group)
{
	ENGINE_VERIFY(m_groups.erase(group), "null driver: destroy_bind_group with a handle of no group (index {}, generation {})", group.index, group.gen);
}

void rnd::driver::null::driver::set_bind_group(std::uint32_t set, bind_group_handle group, std::span<const std::uint32_t> dynamic_offsets)
{
	if (!ENGINE_VERIFY(set < max_bind_sets, "null driver: set_bind_group at set {}, the table has {}", set, max_bind_sets)
		|| !ENGINE_VERIFY(m_groups.find(group), "null driver: set_bind_group with a handle of no group (index {}, generation {})", group.index, group.gen)) {
		return;
	}
	m_bound_sets[set] = { group, std::vector<std::uint32_t>(dynamic_offsets.begin(), dynamic_offsets.end()) };
	++m_calls.bind_group_binds;
	if (m_record_bindings) {
		m_bind_log.push_back({ m_frame, frame_slot(), set, group, m_bound_sets[set].dynamic_offsets });
	}
}

const rnd::driver::buffer_desc* rnd::driver::null::driver::buffer_info(buffer_handle buffer) const
{
	const null_buffer* found = m_buffers.find(buffer);
	return found ? &found->desc : nullptr;
}

const std::vector<std::byte>* rnd::driver::null::driver::buffer_bytes(buffer_handle buffer, std::optional<std::uint32_t> copy) const
{
	const null_buffer* found = m_buffers.find(buffer);
	if (!found) {
		return nullptr;
	}
	const std::size_t index = found->desc.update == buffer_update::per_frame ? copy.value_or(frame_slot()) : 0;
	return index < found->copies.size() ? &found->copies[index] : nullptr;
}

const rnd::driver::bind_group_desc* rnd::driver::null::driver::group_info(bind_group_handle group) const
{
	const null_group* found = m_groups.find(group);
	return found ? &found->desc : nullptr;
}

rnd::driver::pipeline_handle rnd::driver::null::driver::create_pipeline(const pipeline_desc& desc)
{
	return m_pipelines.acquire(desc, [](const pipeline_desc&) { return pipeline_record{}; });
}

void rnd::driver::null::driver::bind_pipeline(pipeline_handle pipeline)
{
	ASSERT_MSG(m_pipelines.find(pipeline), "bind_pipeline with a handle of no pipeline");
	m_bound = pipeline;
	++m_calls.pipeline_binds;
}

const rnd::driver::pipeline_desc& rnd::driver::null::driver::pipeline_info(pipeline_handle pipeline) const
{
	const auto* found = m_pipelines.find(pipeline);
	return found ? found->desc : empty_pipeline_desc();
}
