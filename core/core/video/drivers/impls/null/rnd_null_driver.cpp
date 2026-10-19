#include "rnd_null_driver.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

rnd::driver::null::driver::driver()
	: m_stats(std::make_shared<object_stats>())
{
}

void rnd::driver::null::driver::begin_render_pass(const render_pass_desc& desc)
{
	ASSERT_MSG(!m_in_render_pass, "begin_render_pass inside a render pass");
	if (!desc.backbuffer) {
		const auto key = make_framebuffer_key(desc, [](texture_handle handle) -> std::uint64_t {
			return handle ? static_cast<const texture*>(handle)->framebuffer_object() : 0;
		});
		m_stats->framebuffers.acquire(key, [this] { return ++m_next_framebuffer; });
	}
	m_last_pass = desc;
	m_in_render_pass = true;
	++m_calls.render_pass_begins;
}

void rnd::driver::null::driver::end_render_pass()
{
	ASSERT_MSG(m_in_render_pass, "end_render_pass outside a render pass");
	m_in_render_pass = false;
	++m_calls.render_pass_ends;
}

void rnd::driver::null::driver::draw_indices(const std::unique_ptr<vertex_array_interface>& /*vertices*/, unsigned int /*count*/, unsigned int /*offset*/)
{
	++m_calls.draws;
	++m_draws_by_topology[static_cast<std::size_t>(pipeline_info(m_bound).topology)];
}

void rnd::driver::null::driver::draw_indices(const vertex_array_interface* /*va*/, unsigned int /*count*/, unsigned int /*base_vertex*/, unsigned int /*base_index*/)
{
	++m_calls.draws;
	++m_draws_by_topology[static_cast<std::size_t>(pipeline_info(m_bound).topology)];
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
