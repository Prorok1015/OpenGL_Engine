#include <snk/render/rnd_image_compare.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

namespace {
	constexpr std::size_t pixel_bytes = 4;

	std::optional<std::size_t> byte_count(const rnd::rgba8_view& view)
	{
		if (view.width < 0 || view.height < 0) {
			return std::nullopt;
		}
		return static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.height) * pixel_bytes;
	}
}

std::optional<rnd::image_diff> rnd::compare_rgba8(const rgba8_view& a, const rgba8_view& b, double fuzz_pct)
{
	if (a.width != b.width || a.height != b.height) {
		return std::nullopt;
	}
	const std::optional<std::size_t> bytes = byte_count(a);
	if (!bytes || a.rgba.size() != *bytes || b.rgba.size() != *bytes) {
		return std::nullopt;
	}
	const double threshold = fuzz_pct / 100.0 * 255.0;
	image_diff diff;
	diff.total_pixels = static_cast<std::int64_t>(a.width) * a.height;
	for (std::size_t pixel = 0; pixel < *bytes; pixel += pixel_bytes) {
		for (std::size_t c = 0; c < pixel_bytes; ++c) {
			const int delta = std::abs(std::to_integer<int>(a.rgba[pixel + c]) - std::to_integer<int>(b.rgba[pixel + c]));
			if (static_cast<double>(delta) > threshold) {
				++diff.diff_pixels;
				break;
			}
		}
	}
	if (diff.total_pixels > 0) {
		diff.diff_pct = 100.0 * static_cast<double>(diff.diff_pixels) / static_cast<double>(diff.total_pixels);
	}
	return diff;
}

std::vector<std::byte> rnd::to_rgba8_top_down(std::span<const std::byte> pixels, int width, int height, int channels, bool bottom_up)
{
	std::vector<std::byte> out;
	if (width <= 0 || height <= 0 || channels < 1 || channels > 4) {
		return out;
	}
	const std::size_t w = static_cast<std::size_t>(width);
	const std::size_t h = static_cast<std::size_t>(height);
	const std::size_t ch = static_cast<std::size_t>(channels);
	const std::size_t source_stride = w * ch;
	if (pixels.size() / h < source_stride) {
		return out;
	}
	out.resize(w * h * pixel_bytes);
	for (std::size_t y = 0; y < h; ++y) {
		const std::size_t source_y = bottom_up ? h - 1 - y : y;
		const std::byte* source = pixels.data() + source_y * source_stride;
		std::byte* target = out.data() + y * w * pixel_bytes;
		for (std::size_t x = 0; x < w; ++x, source += ch, target += pixel_bytes) {
			switch (channels) {
			case 1:
				target[0] = target[1] = target[2] = source[0];
				target[3] = std::byte{ 255 };
				break;
			case 2:
				target[0] = target[1] = target[2] = source[0];
				target[3] = source[1];
				break;
			case 3:
				target[0] = source[0];
				target[1] = source[1];
				target[2] = source[2];
				target[3] = std::byte{ 255 };
				break;
			default:
				target[0] = source[0];
				target[1] = source[1];
				target[2] = source[2];
				target[3] = source[3];
				break;
			}
		}
	}
	return out;
}
