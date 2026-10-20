#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/shader/rnd_shader_config_cache.h>
#include <snk/render/rnd_light_data.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/mem_allocator.h>
#include <snk/common/ds/ds_fixed_vector.hpp>
#include <snk/resource/res_tag.h>

namespace rnd {
	// What a camera clears in its target before its first pass draws: the colour and the depth, only the depth
	// (an overlay over what a lower priority left in the colour) or nothing.
	enum class clear_mode : uint8_t { all, depth, none };

	inline constexpr bool clears_color(clear_mode mode) noexcept { return mode == clear_mode::all; }
	inline constexpr bool clears_depth(clear_mode mode) noexcept { return mode != clear_mode::none; }

	struct camera_render_data_t {
		glm::ivec4 viewport; // {left, top, width, height}
		glm::mat4 view_matrix;
		glm::mat4 projection_matrix;
		glm::vec3 view_position = glm::vec3{0.0f};

		ds::fixed_vector<res::tag, 4> color_targets;   // [0] = main color, [1+] = MRT
		res::tag depth_target;                          // depth RT
		res::tag tp_accum_target;                       // WBOIT accumulation
		res::tag tp_reveal_target;                      // WBOIT revealage/weight
		// Packets are rendered in ascending priority; the passes run over all packets of one priority before
		// the next priority starts (render_system::render_frame). The extractor writes the camera's layer.
		int render_priority = 0;
		clear_mode clear = clear_mode::all;
		// Debug view of this camera only: the scene passes (z-prepass, opaque, transparent) draw lines instead of
		// filled triangles. The sky, the composition and the debug lines stay filled.
		bool wireframe = false;
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

		// Call before add() with upper bounds of the matrices and of the skeletons that will be added.
		void reserve(std::size_t matrix_count, std::size_t skeleton_count) {
			matrices.reserve(matrix_count);
			copies.reserve(skeleton_count);
		}

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

	// An index into one of the packet's frame tables (geometries, skins, materials). Plain data, so a draw call
	// stays trivially copyable; the table that resolves it belongs to the packet and lives for the frame.
	template<class Table>
	struct draw_ref {
		static constexpr uint32_t none = ~0u;

		uint32_t index = none;

		constexpr bool is_valid() const noexcept { return index != none; }
		constexpr auto operator<=>(const draw_ref&) const noexcept = default;
	};

	using geometry_id = draw_ref<struct geometry_table>;
	using skin_id = draw_ref<struct skin_table>;
	// Names a material instance: an index into the renderer's material_instance_cache, valid for the frame
	// the draw was extracted in.
	using material_instance_id = draw_ref<struct material_table>;

	// What a draw is sorted within: the list it lives in.
	enum class draw_pass : uint8_t { opaque = 0, transparent = 1 };

	// Sort key layout, high bits first.
	//   opaque:      pass(2) | pipeline(20) | material(22) | depth(20), nearest first: fewer state changes, early-z.
	//   transparent: pass(2) | depth(20)    | pipeline(20) | material(22), farthest first: the blend order.
	// `depth` is the view-space distance along the camera axis, quantized from the top bits of the float (distances
	// are not negative, so the bit pattern grows with the value).
	inline constexpr uint32_t sort_key_depth_bits = 20;
	inline constexpr uint32_t sort_key_pipeline_bits = 20;
	inline constexpr uint32_t sort_key_material_bits = 22;

	inline uint32_t quantize_view_depth(float view_depth)
	{
		const float distance = view_depth > 0.0f ? view_depth : 0.0f; // also catches NaN
		return std::bit_cast<uint32_t>(distance) >> (31 - sort_key_depth_bits);
	}

	inline uint64_t make_sort_key(draw_pass pass, uint32_t pipeline, uint32_t material, float view_depth)
	{
		ASSERT_MSG(pipeline < (1u << sort_key_pipeline_bits), "pipeline {} does not fit the sort key", pipeline);
		ASSERT_MSG(material < (1u << sort_key_material_bits), "material {} does not fit the sort key", material);
		constexpr uint64_t depth_mask = (uint64_t{1} << sort_key_depth_bits) - 1;
		constexpr uint64_t pipeline_mask = (uint64_t{1} << sort_key_pipeline_bits) - 1;
		constexpr uint64_t material_mask = (uint64_t{1} << sort_key_material_bits) - 1;
		const uint64_t pass_bits = static_cast<uint64_t>(pass) << 62;
		const uint64_t depth = quantize_view_depth(view_depth) & depth_mask;
		const uint64_t pipe = pipeline & pipeline_mask;
		const uint64_t mat = material & material_mask;
		if (pass == draw_pass::transparent) {
			const uint64_t far_first = depth_mask - depth;
			return pass_bits | (far_first << 42) | (pipe << sort_key_material_bits) | mat;
		}
		return pass_bits | (pipe << 42) | (mat << sort_key_depth_bits) | depth;
	}

	// `variant` is the pipeline at extraction time: it orders the draws (sort_key) and nothing else. Passes take the
	// program from the instance (render_context::material), which may be compiled again after the draw was made.
	// POD: no tags, strings or pointers. Geometry and skin are ids into the tables of the packet that holds the
	// draw; the material is an id into the material_instance_cache.
	struct draw_call_t {
		uint64_t     sort_key      = 0;
		geometry_id  geometry;
		variant_id   variant;
		material_instance_id material;
		uint32_t     indices_count = 0;
		uint32_t     vx_begin      = 0;
		uint32_t     ind_begin     = 0;
		glm::mat4    transform     = glm::mat4{1.0f};

		// Skinned draws: the skin whose weights the shader reads, and where the skeleton's matrices start in the
		// packet's bone palette and how many there are.
		skin_id  skin;
		uint32_t bone_offset = 0;
		uint32_t bone_count  = 0;
	};
	static_assert(std::is_trivially_copyable_v<draw_call_t>, "a draw call is copied and sorted as plain data");
	static_assert(std::is_standard_layout_v<draw_call_t>);
	static_assert(std::is_trivially_destructible_v<draw_call_t>);

	// Orders draws by sort_key. Draws with equal keys keep their extraction order.
	inline void sort_draws(std::span<draw_call_t> draws)
	{
		std::stable_sort(draws.begin(), draws.end(),
			[](const draw_call_t& a, const draw_call_t& b) { return a.sort_key < b.sort_key; });
	}

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
		material_instance_id material;
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

		// What the draws' ids mean in this packet. Frame temporary like the draws. (Materials are not here: their
		// ids name instances of the material_instance_cache.)
		std::pmr::vector<res::tag> geometries{ds::frame_allocator()};
		std::pmr::vector<res::tag> skins{ds::frame_allocator()};

		const res::tag& geometry_of(const draw_call_t& draw) const { return geometries[draw.geometry.index]; }
		// The skin tag of a skinned draw; the null tag for a draw without one.
		const res::tag& skin_of(const draw_call_t& draw) const { return draw.skin.is_valid() ? skins[draw.skin.index] : res::tag::null; }

		uint32_t opaque_object(std::size_t draw) const { return object_base + static_cast<uint32_t>(draw); }
		uint32_t transparent_object(std::size_t draw) const { return object_base + static_cast<uint32_t>(opaque_draws.size() + draw); }
	};

	// Puts the packets in render order: ascending render_priority, packets of one priority in the order they were
	// extracted in (stable).
	inline void sort_packets_by_priority(std::span<render_packet_t> packets)
	{
		std::stable_sort(packets.begin(), packets.end(),
			[](const render_packet_t& a, const render_packet_t& b) { return a.camera.render_priority < b.camera.render_priority; });
	}

	// Calls visit(group) for each run of packets with one render_priority, lowest first. `packets` must be sorted
	// (sort_packets_by_priority).
	template<class Visit>
	void for_each_priority_group(std::span<render_packet_t> packets, Visit&& visit)
	{
		std::size_t first = 0;
		while (first < packets.size()) {
			const int priority = packets[first].camera.render_priority;
			std::size_t last = first + 1;
			while (last < packets.size() && packets[last].camera.render_priority == priority) {
				++last;
			}
			visit(packets.subspan(first, last - first));
			first = last;
		}
	}
}
