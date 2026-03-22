#pragma once
#include <memory_resource>
#include <optional>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include "shader/rnd_scene_shader_desc.h"
#include "rnd_ssbo_buffer_interface.h"
#include "mem_allocator.h"
#include "ds/ds_fixed_vector.hpp"

namespace rnd {
	struct camera_render_data_t {
		glm::ivec4 viewport; // {left, top, width, height}
		glm::mat4 view_matrix;
		glm::mat4 projection_matrix;
		glm::vec3 view_position = glm::vec3{0.0f};

		ds::fixed_vector<res::tag, 4> color_targets;   // [0] = main color, [1+] = MRT
		res::tag depth_target;                          // depth RT
		res::tag tp_accum_target;                       // WBOIT accumulation
		res::tag tp_reveal_target;                      // WBOIT revealage/weight
		int render_priority = 0;
	};

	// Resolve helpers — fallback to legacy static tags when camera doesn't specify
	inline res::tag resolve_color_target(const camera_render_data_t& cam, std::size_t index = 0)
	{
		if (index < cam.color_targets.size() && cam.color_targets[index].is_valid())
			return cam.color_targets[index];
		static const res::tag fallback = res::tag(res::tag::memory, "__color_scene_rt");
		return fallback;
	}

	inline res::tag resolve_depth_target(const camera_render_data_t& cam)
	{
		if (cam.depth_target.is_valid()) return cam.depth_target;
		static const res::tag fallback = res::tag(res::tag::memory, "__z_prepass_rt");
		return fallback;
	}

	inline res::tag resolve_tp_accum_target(const camera_render_data_t& cam)
	{
		if (cam.tp_accum_target.is_valid()) return cam.tp_accum_target;
		static const res::tag fallback = res::tag(res::tag::memory, "__color_rt_transparent_rt");
		return fallback;
	}

	inline res::tag resolve_tp_reveal_target(const camera_render_data_t& cam)
	{
		if (cam.tp_reveal_target.is_valid()) return cam.tp_reveal_target;
		static const res::tag fallback = res::tag(res::tag::memory, "__waight_rt_transparent_rt");
		return fallback;
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
		const std::vector<glm::mat4>* bone_matrices = nullptr;
	};

	struct debug_line_t {
		glm::vec3 start;
		glm::vec3 end;
		glm::vec4 color = glm::vec4{1.0f, 1.0f, 0.0f, 1.0f};
	};

	struct debug_draw_data_t {
		std::pmr::vector<debug_line_t> lines{ds::frame_allocator()};
	};

	struct render_packet_t {
		camera_render_data_t     camera;
		std::pmr::vector<draw_call_t> opaque_draws{ds::frame_allocator()};
		std::pmr::vector<draw_call_t> transparent_draws{ds::frame_allocator()};
		std::optional<shader_config> skybox_material;
		debug_draw_data_t debug_draws;
	};
}
