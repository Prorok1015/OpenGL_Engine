#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <glm/vec2.hpp>

// The names and the .raw header of the picture files the engine reads (raw_image_adapter, pct_adapter), for writers that
// do not load pictures: the model import stores an embedded texture under one of them (#1077, in the decoder process).
namespace res::image_format
{
	// What raw_image_adapter::raw_image_header holds, in that layout (res_pct_adapter.h checks it).
	struct raw_header
	{
		glm::ivec2 size;
		std::int32_t channels;
	};

	inline constexpr std::size_t raw_header_size = sizeof(raw_header);
	inline constexpr std::string_view raw_extension{ "raw" };
	// pct_adapter::EXTENSIONS: what it decodes, its own name first.
	inline constexpr std::array picture_extensions{ std::string_view{ "pct" }, std::string_view{ "png" }, std::string_view{ "jpeg" },
		std::string_view{ "jpg" } };
	// What one picture may decode to (pct_adapter::max_side, max_decoded_bytes): no side longer than a GPU takes, and no
	// more pixel bytes than this.
	inline constexpr int max_picture_side = 16384;
	inline constexpr std::size_t max_picture_decoded_bytes = std::size_t{ 512 } * 1024 * 1024;
	// rnd::texture_2d_desc's inline pixels (#1204): at most this many bytes and this long a side. Here so the model import
	// and the editor's check of its answer, which link no renderer, take the same limits (#1206).
	inline constexpr std::size_t max_inline_pixel_bytes = 64 * 1024;
	inline constexpr std::uint32_t max_inline_pixel_side = 4096;
}
