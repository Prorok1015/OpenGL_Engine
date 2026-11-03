#pragma once
#include <snk/common/snk_api.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rnd
{
	// RGBA8 pixels, 4 bytes a pixel, rows packed; two views compare only when they share an orientation.
	struct rgba8_view
	{
		int width = 0;
		int height = 0;
		std::span<const std::byte> rgba{};
	};

	struct image_diff
	{
		std::int64_t diff_pixels = 0;
		std::int64_t total_pixels = 0;
		// 100 * diff_pixels / total_pixels; 0 for an empty image.
		double diff_pct = 0.0;
	};

	// A pixel differs when any of its RGBA channels differs by more than `fuzz_pct` % of 255 (ImageMagick's
	// `compare -metric AE -fuzz`, per channel). Nullopt when the sizes differ or a view does not hold
	// width * height * 4 bytes.
	SNK_API std::optional<image_diff> compare_rgba8(const rgba8_view& a, const rgba8_view& b, double fuzz_pct);

	// Expands 1 (grey), 2 (grey + alpha), 3 (RGB) or 4 (RGBA) channels to RGBA8; a missing alpha is 255.
	// `bottom_up`: the source's first row is the bottom of the picture, the result puts the top row first.
	// Empty when the channel count is not 1..4, a side is not positive or `pixels` holds less than the picture.
	SNK_API std::vector<std::byte> to_rgba8_top_down(std::span<const std::byte> pixels, int width, int height, int channels, bool bottom_up);
}
