#pragma once
#include <memory_resource>
#include <optional>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include "shader/rnd_scene_shader_desc.h"
#include "rnd_ssbo_buffer_interface.h"
#include "mem_allocator.h"

namespace rnd {
	struct camera_render_data_t {
		glm::ivec4 viewport; // {left, top, width, height}
		glm::mat4 view_matrix;
		glm::mat4 projection_matrix;
		glm::vec3 view_position = glm::vec3{0.0f};
		res::tag target_texture_tag;
		int render_priority = 0;
	};

	struct draw_call_t {
		uint64_t     sort_key      = 0;
		res::tag     geometry_tag;
		shader_config material;
		uint32_t     indices_count = 0;
		uint32_t     vx_begin      = 0;
		uint32_t     ind_begin     = 0;
		glm::mat4    transform     = glm::mat4{1.0f};

		res::tag skinning_tag;
		std::pmr::vector<glm::mat4> bone_matrices{ds::frame_allocator()};
	};

	struct render_packet_t {
		camera_render_data_t     camera;
		std::pmr::vector<draw_call_t> opaque_draws{ds::frame_allocator()};
		std::pmr::vector<draw_call_t> transparent_draws{ds::frame_allocator()};
		std::optional<shader_config> skybox_material;
	};
}
