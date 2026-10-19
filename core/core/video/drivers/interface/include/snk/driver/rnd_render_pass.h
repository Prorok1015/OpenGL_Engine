#pragma once
#include <snk/common/ds/ds_fixed_vector.hpp>

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>

namespace rnd::driver
{
	class texture_interface;

	// Until typed handles (RHI R7) a texture handle is the texture object itself.
	using texture_handle = texture_interface*;

	enum class load_op : uint8_t
	{
		load,		// keep the contents the attachment has
		clear,		// clear it to the attachment's clear value
		dont_care	// contents are undefined at the start of the pass
	};

	enum class store_op : uint8_t
	{
		store,		// keep what the pass wrote
		dont_care	// contents are undefined after the pass
	};

	struct attachment_desc
	{
		// Null stands for the swapchain buffer when render_pass_desc::backbuffer is set; elsewhere a null
		// color leaves its draw buffer slot unused.
		texture_handle texture = nullptr;
		uint32_t mip = 0;
		uint32_t layer = 0;	// array layer, cube face or 3D slice
		load_op load = load_op::load;
		store_op store = store_op::store;
		glm::vec4 clear_color{ 0.f };
		float clear_depth = 1.f;
		uint8_t clear_stencil = 0;
	};

	struct render_pass_desc
	{
		static constexpr std::size_t max_colors = 8;

		ds::fixed_vector<attachment_desc, max_colors> colors;
		std::optional<attachment_desc> depth_stencil;
		// Draw into the window's buffers. Attachments are then optional and carry a null texture: they only
		// give the load and store ops of the swapchain's color (colors[0]) and depth (depth_stencil).
		bool backbuffer = false;
		// {left, top, width, height} in the convention of set_viewport. Limits the clears of the pass and is
		// the viewport the pass starts with; {0,0,0,0} is the whole target and leaves the viewport alone.
		glm::ivec4 render_area{ 0 };
		const char* debug_name = nullptr;	// label for GPU captures and validation messages; must outlive the pass
	};
}
