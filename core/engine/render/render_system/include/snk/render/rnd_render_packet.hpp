#pragma once
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/shader/rnd_shader_config_cache.h>
#include <snk/render/rnd_light_data.hpp>
#include <snk/common/mem_allocator.h>
#include <snk/common/ds/ds_fixed_vector.hpp>

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

	// The bone matrices of the skeletons a packet's draws use, copied out of the ECS during extract. A skeleton
	// shared by several draws is copied once. Frame temporary: the vectors use the frame allocator.
	struct bone_palette_t {
		std::pmr::vector<glm::mat4> matrices{ds::frame_allocator()};
		// Skeletons copied so far: the address of the source palette and where its copy starts in `matrices`.
		std::pmr::vector<std::pair<const void*, uint32_t>> copies{ds::frame_allocator()};

		// Call before add() with an upper bound of the matrices that will be added, so the copies never reallocate.
		void reserve(std::size_t matrix_count) { matrices.reserve(matrix_count); }

		// The index of the first matrix of `palette` in `matrices`, copying it on its first use.
		uint32_t add(const void* source, std::span<const glm::mat4> palette) {
			for (const auto& [copied, first] : copies) {
				if (copied == source) {
					return first;
				}
			}
			const uint32_t first = static_cast<uint32_t>(matrices.size());
			matrices.insert(matrices.end(), palette.begin(), palette.end());
			copies.emplace_back(source, first);
			return first;
		}
	};

	struct draw_call_t {
		uint64_t     sort_key      = 0;
		res::tag     geometry_tag;
		variant_id   variant;
		// Valid from extract to the end of render in the same frame: owned by compiled_material_component or by the extractor's retired list, which the next extract clears.
		const compiled_material* material = nullptr;
		uint32_t     indices_count = 0;
		uint32_t     vx_begin      = 0;
		uint32_t     ind_begin     = 0;
		glm::mat4    transform     = glm::mat4{1.0f};

		// Skinned draws: the skin whose weights the shader reads and where the skeleton's matrices start in the
		// packet's bone palette.
		res::tag skinning_tag;
		uint32_t bone_offset = 0;
	};

	struct debug_line_t {
		glm::vec3 start;
		glm::vec3 end;
		glm::vec4 color = glm::vec4{1.0f, 1.0f, 0.0f, 1.0f};
	};

	struct debug_draw_data_t {
		std::pmr::vector<debug_line_t> lines{ds::frame_allocator()};
	};

	struct skybox_draw_t {
		variant_id variant;
		const compiled_material* material = nullptr;
		std::optional<glm::mat4> world;
	};

	struct render_packet_t {
		camera_render_data_t     camera;
		scene_lights_t           lights;
		std::pmr::vector<draw_call_t> opaque_draws{ds::frame_allocator()};
		std::pmr::vector<draw_call_t> transparent_draws{ds::frame_allocator()};
		std::optional<skybox_draw_t> skybox_material;
		debug_draw_data_t debug_draws;
		bone_palette_t bones;

		// Where the packet's elements start in the frame's object ring and bone buffer. Set by frame_upload_pass,
		// which puts the opaque draws first and the transparent ones after them.
		uint32_t object_base = 0;
		uint32_t bone_base = 0;

		uint32_t opaque_object(std::size_t draw) const { return object_base + static_cast<uint32_t>(draw); }
		uint32_t transparent_object(std::size_t draw) const { return object_base + static_cast<uint32_t>(opaque_draws.size() + draw); }
	};
}
