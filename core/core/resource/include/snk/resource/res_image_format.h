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
}
