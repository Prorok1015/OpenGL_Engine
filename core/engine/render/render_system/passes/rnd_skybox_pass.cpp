#include "rnd_skybox_pass.h"
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_system.h>
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"

namespace rnd {
	struct skybox_vertex {
		glm::vec3 position;
		glm::vec3 normal;
		glm::vec2 uv;
	};

	static const skybox_vertex cube_vertices[8] = {
		{{-1.f, -1.f, -1.f}, {0.f, 0.f, 0.f}, {0.f, 0.f}},
		{{-1.f,  1.f, -1.f}, {0.f, 0.f, 0.f}, {0.f, 1.f}},
		{{ 1.f,  1.f, -1.f}, {0.f, 0.f, 0.f}, {1.f, 1.f}},
		{{ 1.f, -1.f, -1.f}, {0.f, 0.f, 0.f}, {1.f, 0.f}},
		{{-1.f, -1.f,  1.f}, {0.f, 0.f, 0.f}, {0.f, 0.f}},
		{{ 1.f, -1.f,  1.f}, {0.f, 0.f, 0.f}, {1.f, 0.f}},
		{{ 1.f,  1.f,  1.f}, {0.f, 0.f, 0.f}, {1.f, 1.f}},
		{{-1.f,  1.f,  1.f}, {0.f, 0.f, 0.f}, {0.f, 1.f}}
	};

	static const unsigned int cube_indices[36] = {
		0, 1, 3,  3, 1, 2,
		4, 5, 7,  7, 5, 6,
		0, 4, 1,  1, 4, 7,
		2, 6, 3,  3, 6, 5,
		1, 7, 2,  2, 7, 6,
		0, 3, 4,  4, 3, 5
	};

	skybox_pass::skybox_pass() {}

	void skybox_pass::execute(frame_context& context, driver::driver_interface& drv) {
		PROFILE_SCOPE("Pass.Skybox");
		PROFILE_GPU_SCOPE("Pass.Skybox");
		if (!context.data.has_value<std::pmr::vector<rnd::render_packet_t>>()) {
			return;
		}

		if (!m_va) {
			m_va = drv.create_vertex_array();
			
			m_vbo = drv.create_buffer();
			m_vbo->reserve(sizeof(cube_vertices));
			m_vbo->set_layout(
				{
					{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position"},
					{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal"},
					{rnd::driver::SHADER_DATA_TYPE::VEC2_F, "texture_position"}
				}
			);
			m_va->add_vertex_buffer(m_vbo);

			m_ibo = drv.create_buffer();
			m_va->set_index_buffer(m_ibo);
		}

		auto& packets = context.data.require<std::pmr::vector<rnd::render_packet_t>>();

		auto& txm_manager = *context.textures;
		rnd::global_params common_matrix;

		for (const auto& packet : packets) {
			if (!packet.skybox_material.has_value()) {
				continue;
			}

			int vp_width = packet.camera.viewport.z;
			int vp_height = packet.camera.viewport.w;

			if (vp_width < 1 || vp_height < 1) continue;

			const auto target = rnd::select_camera_target(packet.camera);
			const bool offscreen = target.kind == rnd::render_target_kind::texture;
			res::tag depth_tag = rnd::resolve_depth_target(packet.camera);
			auto color_rt = offscreen ? txm_manager.find(target.texture) : nullptr;
			if (offscreen && !color_rt) continue;

			rnd::driver::render_state state;
			state.depth.enabled = true;
			state.depth.write_mask = false;
			state.depth.test_func = rnd::driver::depth_state::func::LEQUAL;
			drv.set_render_state(state);

			if (offscreen) {
				drv.push_frame_buffer();
				drv.set_render_target(color_rt, txm_manager.find(depth_tag));
			}
			
			common_matrix.view = packet.camera.view_matrix;
			common_matrix.projection = packet.camera.projection_matrix;
			common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
			context.shaders->update_global_uniform(common_matrix);
			drv.set_viewport(glm::ivec4{packet.camera.viewport.x, packet.camera.viewport.y, vp_width, vp_height});

			m_vbo->set_data_ptr(cube_vertices, 8);
			m_ibo->set_data_ptr(cube_indices, 36);

			const auto& sky = *packet.skybox_material;
			rnd::configure_pass(context, sky.variant, *sky.material, sky.world ? &*sky.world : nullptr);
			drv.draw_indices(m_va, rnd::RENDER_MODE::TRIANGLE, 36);

			if (offscreen) {
				drv.pop_frame_buffer();
			}
		}
	}
}
