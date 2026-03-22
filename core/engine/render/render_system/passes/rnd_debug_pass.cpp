#include "rnd_debug_pass.h"
#include "rnd_render_packet.hpp"
#include "rnd_render_system.h"
#include "rnd_buffer_layout.h"
#include "eng_profiler.h"

namespace rnd {

struct debug_vertex_t {
	glm::vec3 position;
	glm::vec4 color;
};

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

	m_shader = drv.create_shader(cfg.load());
}

void debug_pass::execute(frame_context& context, driver::driver_interface& drv)
{
	PROFILE_SCOPE("Pass.Debug");
	if (!context.data.has_value<std::pmr::vector<rnd::render_packet_t>>())
		return;

	auto& packets = context.data.require<std::pmr::vector<rnd::render_packet_t>>();

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

	static res::tag color_rt_tag = res::tag(res::tag::memory, "__color_scene_rt");
	static res::tag z_pass_tag = res::tag(res::tag::memory, "__z_prepass_rt");

	auto& txm_manager = rnd::get_system().get_texture_manager();
	rnd::global_params common_matrix;

	for (const auto& packet : packets) {
		if (packet.debug_draws.lines.empty()) continue;

		int vp_width = packet.camera.viewport.z;
		int vp_height = packet.camera.viewport.w;
		if (vp_width < 1 || vp_height < 1) continue;

		res::tag target_tag = packet.camera.target_texture_tag;
		if (!target_tag.is_valid()) {
			target_tag = color_rt_tag;
		}
		auto color_rt = txm_manager.find(target_tag);
		if (!color_rt) continue;

		driver::render_state state;
		state.depth.enabled = false;
		state.depth.write_mask = false;
		drv.set_render_state(state);

		drv.push_frame_buffer();
		drv.set_render_target(color_rt, txm_manager.find(z_pass_tag));

		common_matrix.view = packet.camera.view_matrix;
		common_matrix.projection = packet.camera.projection_matrix;
		common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
		common_matrix.time = 0.f;

		rnd::get_system().get_shader_manager().update_global_uniform(common_matrix);
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

		m_shader->use();
		drv.set_line_size(1.0f);
		drv.draw_indices(m_vao, driver::RENDER_MODE::LINE,
			static_cast<unsigned int>(indices.size()), 0);

		drv.pop_frame_buffer();
	}
}
} // namespace rnd
