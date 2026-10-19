#pragma once
#include <snk/driver/rnd_bind_group.h>
#include <snk/driver/rnd_render_pass.h>
#include <snk/driver/rnd_shader_interface.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/common/ds/ds_fixed_vector.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace rnd::driver
{
	// Depth state configuration
	struct depth_state {
		enum class func {
			LESS,
			LEQUAL,
			EQUAL,
			ALWAYS,
			GREATER,
			GEQUAL,
			NOTEQUAL,
			NEVER
		};

		bool enabled = false;
		bool write_mask = true;
		func test_func = func::LESS;

		friend bool operator==(const depth_state&, const depth_state&) = default;
	};

	// Blend state configuration
	struct blend_state {
		enum class factor {
			ZERO,
			ONE,
			SRC_COLOR,
			ONE_MINUS_SRC_COLOR,
			DST_COLOR,
			ONE_MINUS_DST_COLOR,
			SRC_ALPHA,
			ONE_MINUS_SRC_ALPHA,
			DST_ALPHA,
			ONE_MINUS_DST_ALPHA
		};

		enum class equation {
			ADD,
			SUBTRACT,
			REVERSE_SUBTRACT,
			MIN,
			MAX
		};

		bool enabled = false;
		factor src_factor = factor::ONE;
		factor dst_factor = factor::ZERO;
		equation blend_eq = equation::ADD;

		friend bool operator==(const blend_state&, const blend_state&) = default;
	};

	// Face culling configuration
	struct face_culling_state {
		enum class mode {
			FRONT,
			BACK,
			FRONT_AND_BACK
		};

		bool enabled = false;
		mode cull_mode = mode::BACK;

		friend bool operator==(const face_culling_state&, const face_culling_state&) = default;
	};

	// How the indices of a draw assemble into primitives.
	enum class RENDER_MODE
	{
		TRIANGLE,
		TRIANGLE_STRIP,
		LINE,
		LINE_STRIP,
		POINT,
	};

	using texture_format = texture_header::TYPE;

	// The window's buffers have no texture to ask: a pass on the backbuffer renders to these.
	inline constexpr texture_format backbuffer_color_format = texture_format::RGBA8;
	inline constexpr texture_format backbuffer_depth_format = texture_format::D24_S8;

	// A shader as a pipeline names it: the object to bind, and its serial, which tells it from a later shader
	// that happens to live at the same address.
	struct shader_ref
	{
		const shader_interface* object = nullptr;
		std::uint64_t serial = 0;

		shader_ref() = default;
		shader_ref(const shader_interface* shader) noexcept : object(shader), serial(shader ? shader->serial() : 0) {}

		explicit operator bool() const noexcept { return object != nullptr; }
		friend bool operator==(const shader_ref&, const shader_ref&) = default;
	};

	struct pipeline_tag;
	using pipeline_handle = handle<pipeline_tag>;

	// Everything that is fixed while a draw runs: program, fixed-function state, and the formats of the
	// targets it renders to. Creating a pipeline copies the description; the pipeline cannot change afterwards.
	struct pipeline_desc
	{
		static constexpr std::size_t max_targets = render_pass_desc::max_colors;

		// The program. The shader must outlive every use of the pipeline.
		shader_ref shader;
		RENDER_MODE topology = RENDER_MODE::TRIANGLE;
		depth_state depth;
		face_culling_state cull;
		// One entry per color target, in draw buffer order; targets past the end do not blend.
		ds::fixed_vector<blend_state, max_targets> blend;
		// Formats of the pass this pipeline is for; an unused color slot has none. No depth format = no depth attachment.
		ds::fixed_vector<std::optional<texture_format>, max_targets> color_formats;
		std::optional<texture_format> depth_format;
		// The bind group layout the pipeline expects at each set; a null handle (or a set past the end) is a set
		// the pipeline does not use. A draw needs a group of exactly that layout bound at every set it names.
		ds::fixed_vector<bind_group_layout_handle, max_bind_sets> set_layouts;

		friend bool operator==(const pipeline_desc&, const pipeline_desc&) = default;
	};

	struct pipeline_desc_hash
	{
		std::size_t operator()(const pipeline_desc& desc) const noexcept
		{
			std::uint64_t h = 0xcbf29ce484222325ull;
			const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; };
			mix(desc.shader.serial);
			mix(static_cast<std::uint64_t>(desc.topology));
			mix(static_cast<std::uint64_t>(desc.depth.enabled) | static_cast<std::uint64_t>(desc.depth.write_mask) << 1
				| static_cast<std::uint64_t>(desc.depth.test_func) << 8);
			mix(static_cast<std::uint64_t>(desc.cull.enabled) | static_cast<std::uint64_t>(desc.cull.cull_mode) << 8);
			mix(desc.blend.size());
			for (const blend_state& b : desc.blend) {
				mix(static_cast<std::uint64_t>(b.enabled) | static_cast<std::uint64_t>(b.src_factor) << 8
					| static_cast<std::uint64_t>(b.dst_factor) << 16 | static_cast<std::uint64_t>(b.blend_eq) << 24);
			}
			mix(desc.color_formats.size());
			for (const auto& format : desc.color_formats) {
				mix(format ? static_cast<std::uint64_t>(*format) + 1 : 0);
			}
			mix(desc.depth_format ? static_cast<std::uint64_t>(*desc.depth_format) + 1 : 0);
			mix(desc.set_layouts.size());
			for (const bind_group_layout_handle& layout : desc.set_layouts) {
				mix(static_cast<std::uint64_t>(layout.index) | static_cast<std::uint64_t>(layout.gen) << 32);
			}
			return static_cast<std::size_t>(h);
		}
	};
}
