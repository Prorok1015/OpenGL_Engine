#include "rnd_debug_pass.h"
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_context.h>
#include "rnd_pass_pipelines.h"
#include <snk/driver/rnd_buffer_layout.h>
#include <snk/common/eng_profiler.h>

namespace rnd {

struct debug_vertex_t {
	glm::vec3 position;
	glm::vec4 color;
};

debug_pass::debug_pass(res::resource_system& res_sys)
	: m_res_sys(res_sys)
{
}

void debug_pass::ensure_resources(driver::driver_interface& drv)
{
	if (m_vao) return;

	m_vao = drv.create_vertex_array();

	m_vertex_buffer = drv.create_buffer();
	m_vertex_buffer->set_layout({
		{driver::SHADER_DATA_TYPE::VEC3_F, "aPos"},
		{driver::SHADER_DATA_TYPE::VEC4_F, "aColor"},
	});

	m_index_buffer = drv.create_buffer();

	m_vao->add_vertex_buffer(m_vertex_buffer);
	m_vao->set_index_buffer(m_index_buffer);

	shader_config cfg;
	cfg.cdata.program = shader_config::shader_program_data::build()
		.set_vertex_shader(res::tag::make("shaders/debug_line.vert"))
		.set_fragment_shader(res::tag::make("shaders/debug_line.frag"));

	const auto source = cfg.load(m_res_sys, drv.bindings());
	if (source.ok()) {
		m_shader = drv.create_shader(source.headers);
	}
}

void debug_pass::execute(render_context& context, driver::driver_interface& drv)
{
	PROFILE_SCOPE("Pass.Debug");
	const auto packets = context.packets();
	if (packets.empty())
		return;

	bool has_any_lines = false;
	for (const auto& packet : packets) {
		if (!packet.debug_draws.lines.empty()) {
			has_any_lines = true;
			break;
		}
	}
	if (!has_any_lines) return;

	ensure_resources(drv);
	if (!m_shader) return;

	rnd::global_params common_matrix;

	for (const auto& packet : packets) {
		if (packet.debug_draws.lines.empty()) continue;

		int vp_width = packet.camera.viewport.z;
		int vp_height = packet.camera.viewport.w;
		if (vp_width < 1 || vp_height < 1) continue;

		const auto target = rnd::select_camera_target(packet.camera);
		const bool offscreen = target.kind == rnd::render_target_kind::texture;
		res::tag depth_tag = rnd::resolve_depth_target(packet.camera);
		auto color_rt = offscreen ? context.find_texture(target.texture) : nullptr;
		if (offscreen && !color_rt) continue;

		driver::render_pass_desc pass;
		pass.debug_name = "debug";
		if (offscreen) {
			pass.colors.push_back({.texture = rnd::driver::handle_of(color_rt)});
			if (auto* depth = context.find_texture(depth_tag)) {
				pass.depth_stencil = driver::attachment_desc{.texture = rnd::driver::handle_of(depth)};
			}
		} else {
			pass.backbuffer = true;
		}
		drv.begin_render_pass(pass);

		driver::pipeline_desc state;
		state.depth.enabled = false;
		state.depth.write_mask = false;
		state.topology = driver::RENDER_MODE::LINE;
		driver::set_target_formats(drv, state, pass);
		state.set_layouts = context.layouts().frame_only();
		pass_pipelines pipelines(drv, context, state);

		common_matrix.view = packet.camera.view_matrix;
		common_matrix.projection = packet.camera.projection_matrix;
		common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
		common_matrix.time = 0.f;

		context.write_view(common_matrix);
		context.bind_frame();
		drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
			packet.camera.viewport.y, vp_width, vp_height});

		const auto& lines = packet.debug_draws.lines;
		std::size_t vertex_count = lines.size() * 2;

		std::vector<debug_vertex_t> vertices;
		vertices.reserve(vertex_count);
		std::vector<uint32_t> indices;
		indices.reserve(vertex_count);

		for (const auto& line : lines) {
			uint32_t base = static_cast<uint32_t>(vertices.size());
			vertices.push_back({line.start, line.color});
			vertices.push_back({line.end, line.color});
			indices.push_back(base);
			indices.push_back(base + 1);
		}

		std::size_t vb_size = vertices.size() * sizeof(debug_vertex_t);
		std::size_t ib_size = indices.size() * sizeof(uint32_t);
		if (vb_size > m_vb_capacity) {
			m_vb_capacity = vb_size * 2;
			m_vertex_buffer->reserve(m_vb_capacity);
		}
		m_vertex_buffer->set_data(vertices.data(), vb_size, driver::BUFFER_BINDING::DYNAMIC);
		if (ib_size > m_ib_capacity) {
			m_ib_capacity = ib_size * 2;
			m_index_buffer->reserve(m_ib_capacity);
		}
		m_index_buffer->set_data(indices.data(), ib_size, driver::BUFFER_BINDING::DYNAMIC);

		if (pipelines.bind(m_shader.get())) {
			drv.set_line_size(1.0f);
			drv.draw_indices(m_vao, static_cast<unsigned int>(indices.size()), 0);
		}

		drv.end_render_pass();
	}
}
} // namespace rnd
