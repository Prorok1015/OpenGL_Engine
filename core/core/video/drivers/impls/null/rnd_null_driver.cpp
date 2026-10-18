#include "rnd_null_driver.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

rnd::driver::null::driver::driver()
	: m_stats(std::make_shared<object_stats>())
{
}

void rnd::driver::null::driver::push_frame_buffer()
{
	++m_frame_buffer_depth;
	++m_calls.push_frame_buffers;
}

void rnd::driver::null::driver::pop_frame_buffer()
{
	if (m_frame_buffer_depth == 1) {
		ASSERT_FAIL("Trying to pop backbuffer");
		return;
	}
	--m_frame_buffer_depth;
	++m_calls.pop_frame_buffers;
}

void rnd::driver::null::driver::set_render_target(texture_interface* /*color*/, texture_interface* /*depth_stencil*/)
{
	++m_calls.render_target_sets;
}

void rnd::driver::null::driver::set_render_targets(std::vector<texture_interface*> /*colors*/, texture_interface* /*depth_stencil*/)
{
	++m_calls.render_target_sets;
}

void rnd::driver::null::driver::clear(clear_flags /*flags*/)
{
	++m_calls.clears;
}

void rnd::driver::null::driver::clear(clear_flags /*flags*/, glm::vec4 /*color*/)
{
	++m_calls.clears;
}

void rnd::driver::null::driver::clear(clear_flags /*flags*/, const std::vector<glm::vec4>& /*colors*/)
{
	++m_calls.clears;
}

void rnd::driver::null::driver::draw_indices(const std::unique_ptr<vertex_array_interface>& /*vertices*/, RENDER_MODE /*render_mode*/, unsigned int /*count*/, unsigned int /*offset*/)
{
	++m_calls.draws;
}

void rnd::driver::null::driver::draw_indices(const vertex_array_interface* /*va*/, RENDER_MODE /*rm*/, unsigned int /*count*/, unsigned int /*base_vertex*/, unsigned int /*base_index*/)
{
	++m_calls.draws;
}

std::unique_ptr<rnd::driver::shader_interface> rnd::driver::null::driver::create_shader(const std::vector<shader_header>& headers)
{
	const std::string error = validate_shader_headers(headers);
	if (!error.empty()) {
		egLOG_ERROR("shader/load", "null driver rejected the shader: {}", error);
		return nullptr;
	}
	return std::make_unique<shader>(m_stats, headers);
}

std::unique_ptr<rnd::driver::texture_interface> rnd::driver::null::driver::create_texture(const texture_header& header)
{
	return std::make_unique<texture>(m_stats, header);
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::driver::null::driver::create_vertex_array()
{
	return std::make_unique<vertex_array>(m_stats);
}

std::unique_ptr<rnd::driver::buffer_interface> rnd::driver::null::driver::create_buffer()
{
	return std::make_unique<buffer>(m_stats);
}

std::unique_ptr<rnd::driver::uniform_buffer_interface> rnd::driver::null::driver::create_uniform_buffer(std::size_t size, std::size_t binding)
{
	return std::make_unique<uniform_buffer>(m_stats, size, binding);
}

std::unique_ptr<rnd::driver::ssbo_buffer_interface> rnd::driver::null::driver::create_ssbo_buffer(std::size_t size, std::size_t binding)
{
	return std::make_unique<ssbo_buffer>(m_stats, size, binding);
}

void rnd::driver::null::driver::set_render_state(const render_state& state)
{
	m_render_state = state;
	++m_calls.render_state_sets;
}
