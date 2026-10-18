#pragma once
#include <memory_resource>
#include <optional>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include "shader/rnd_scene_shader_desc.h"
#include <snk/driver/rnd_ssbo_buffer_interface.h>
#include "rnd_light_data.hpp"
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

	// Where a camera's main color goes: straight to the window's default framebuffer, or into a texture.
	enum class render_target_kind : uint8_t { backbuffer, texture };

	struct camera_target {
		render_target_kind kind = render_target_kind::backbuffer;
		res::tag texture; // valid only for kind == texture
	};

	// A camera without a (valid) color target draws to the backbuffer; the editor redirects such
	// cameras to textures so it can show them in its viewport.
	inline bool targets_backbuffer(const ds::fixed_vector<res::tag, 4>& color_targets)
	{
		return color_targets.empty() || !color_targets[0].is_valid();
	}

	inline camera_target select_camera_target(const camera_render_data_t& cam)
	{
		if (!targets_backbuffer(cam.color_targets))
			return { render_target_kind::texture, cam.color_targets[0] };
		return { render_target_kind::backbuffer, {} };
	}

	// The viewport a camera renders with: its own if it has one, else (backbuffer cameras only)
	// the whole backbuffer. nullopt when there is nothing to render into.
	inline std::optional<glm::ivec4> effective_camera_viewport(
		const glm::ivec4& own_viewport, bool targets_the_backbuffer, const glm::ivec2& backbuffer_size)
	{
		if (own_viewport.z >= 1 && own_viewport.w >= 1)
			return own_viewport;
		if (targets_the_backbuffer && backbuffer_size.x >= 1 && backbuffer_size.y >= 1)
			return glm::ivec4{ 0, 0, backbuffer_size.x, backbuffer_size.y };
		return std::nullopt;
	}

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
		scene_lights_t           lights;
		std::pmr::vector<draw_call_t> opaque_draws{ds::frame_allocator()};
		std::pmr::vector<draw_call_t> transparent_draws{ds::frame_allocator()};
		std::optional<shader_config> skybox_material;
		debug_draw_data_t debug_draws;
	};
}
