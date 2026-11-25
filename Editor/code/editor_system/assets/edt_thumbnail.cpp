#include "edt_thumbnail.h"
#include <snk/image/img_image.h>
#include <snk/platform/plt_beneath.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <limits>
#include <string_view>

namespace {

	// One pixel of the picture as RGBA, from 1 (grey), 2 (grey, alpha), 3 or 4 channels.
	void rgba_at(const unsigned char* pixel, int channels, unsigned char out[4])
	{
		switch (channels) {
		case 1: out[0] = out[1] = out[2] = pixel[0]; out[3] = 255; break;
		case 2: out[0] = out[1] = out[2] = pixel[0]; out[3] = pixel[1]; break;
		case 3: out[0] = pixel[0]; out[1] = pixel[1]; out[2] = pixel[2]; out[3] = 255; break;
		default: out[0] = pixel[0]; out[1] = pixel[1]; out[2] = pixel[2]; out[3] = pixel[3]; break;
		}
	}

}

std::optional<edt::thumbnail_pixels> edt::downscale_to_thumbnail(const unsigned char* data, int width, int height, int channels,
	bool bottom_up, int max_side)
{
	if (!data || width <= 0 || height <= 0 || channels < 1 || channels > 4 || max_side <= 0) {
		return std::nullopt;
	}
	const int longest = (std::max)(width, height);
	const int out_w = longest <= max_side ? width : (std::max)(1, static_cast<int>(static_cast<long long>(width) * max_side / longest));
	const int out_h = longest <= max_side ? height : (std::max)(1, static_cast<int>(static_cast<long long>(height) * max_side / longest));

	thumbnail_pixels out;
	out.width = out_w;
	out.height = out_h;
	out.rgba.assign(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4, 0);

	for (int y = 0; y < out_h; ++y) {
		// The rows of the picture this output row covers, counted from the top of the picture.
		const int top_a = static_cast<int>(static_cast<long long>(y) * height / out_h);
		const int top_b = (std::max)(top_a + 1, static_cast<int>(static_cast<long long>(y + 1) * height / out_h));
		for (int x = 0; x < out_w; ++x) {
			const int left_a = static_cast<int>(static_cast<long long>(x) * width / out_w);
			const int left_b = (std::max)(left_a + 1, static_cast<int>(static_cast<long long>(x + 1) * width / out_w));
			unsigned long long sum[4] = { 0, 0, 0, 0 };
			unsigned long long count = 0;
			for (int sy = top_a; sy < top_b && sy < height; ++sy) {
				const int row = bottom_up ? height - 1 - sy : sy;
				for (int sx = left_a; sx < left_b && sx < width; ++sx) {
					unsigned char rgba[4];
					rgba_at(data + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(sx)) * static_cast<std::size_t>(channels), channels, rgba);
					for (int k = 0; k < 4; ++k) sum[k] += rgba[k];
					++count;
				}
			}
			unsigned char* target = out.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(out_w) + static_cast<std::size_t>(x)) * 4;
			for (int k = 0; k < 4; ++k) {
				target[k] = count ? static_cast<unsigned char>((sum[k] + count / 2) / count) : 0;
			}
		}
	}
	return out;
}

bool edt::thumbnail_decodable(std::string_view path)
{
	std::string lower{ path };
	std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (const std::string_view suffix : { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr" }) {
		if (lower.ends_with(suffix)) {
			return true;
		}
	}
	return false;
}

edt::thumbnail_decoded edt::decode_thumbnail_file(const std::filesystem::path& root, const std::filesystem::path& relative)
{
	std::optional<platform::beneath::file_stamp> ignored;
	return decode_thumbnail_file(root, relative, ignored);
}

edt::thumbnail_decoded edt::decode_thumbnail_file(const std::filesystem::path& root, const std::filesystem::path& relative,
	std::optional<platform::beneath::file_stamp>& read_stamp)
{
	thumbnail_decoded out;
	read_stamp.reset();
	platform::beneath::read_request request;
	request.root = root;
	request.folder = relative.parent_path();
	request.leaf = relative.filename();
	request.max_bytes = k_thumbnail_max_file_bytes;
	std::vector<std::byte> bytes;
	const platform::beneath::read_result read = platform::beneath::read_beneath(request, bytes);
	switch (read.status) {
	case platform::beneath::read_status::ok: break;
	case platform::beneath::read_status::too_large:
		out.error = std::format("the file is larger than {} MiB", k_thumbnail_max_file_bytes / (1024 * 1024));
		return out;
	case platform::beneath::read_status::not_regular:
		out.error = "it is not a regular file";
		return out;
	case platform::beneath::read_status::missing:
		out.error = "the file is gone";
		return out;
	default:
		out.error = read.error.empty() ? std::string{ "it cannot be read" } : read.error;
		return out;
	}

	read_stamp = read.stamp;
	const auto* encoded = reinterpret_cast<const unsigned char*>(bytes.data());
	const int length = static_cast<int>((std::min)(bytes.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
	int width = 0;
	int height = 0;
	int channels = 0;
	if (!img::image::info_from_memory(encoded, length, width, height, channels)) {
		out.error = "the picture header cannot be read";
		return out;
	}
	if (width <= 0 || height <= 0 || width > k_thumbnail_max_picture_side || height > k_thumbnail_max_picture_side
		|| static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > k_thumbnail_max_picture_pixels) {
		out.error = std::format("the picture is too large ({}x{})", width, height);
		return out;
	}
	const img::image picture = img::image::read_from_memory(encoded, length);
	if (!picture.data() || picture.width() <= 0 || picture.height() <= 0) {
		out.error = "the picture cannot be decoded";
		return out;
	}
	out.pixels = downscale_to_thumbnail(picture.data(), picture.width(), picture.height(), picture.channels(),
		picture.flipped() == img::image_flip::vertical);
	if (!out.pixels) {
		out.error = "the picture has an unsupported format";
	}
	return out;
}
