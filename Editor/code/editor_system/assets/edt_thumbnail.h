#pragma once
#include <snk/platform/plt_beneath.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace edt
{
	// A small preview of a picture for the asset browser (#1192): RGBA8, rows from the top, at most k_thumbnail_side on a side.
	inline constexpr int k_thumbnail_side = 64;
	// What a picture may be before it is decoded at all: the header says, nothing is allocated for more (#928 has the
	// decoder's own budget; this one refuses a preview of an absurd picture with a reason).
	inline constexpr int k_thumbnail_max_picture_side = 16384;
	inline constexpr std::uint64_t k_thumbnail_max_picture_pixels = 16ull * 1024 * 1024; // 4096 x 4096: with 4 decodes at once, bounds the memory
	inline constexpr std::uint64_t k_thumbnail_max_file_bytes = 64ull * 1024 * 1024;

	struct thumbnail_pixels
	{
		int width = 0;
		int height = 0;
		std::vector<unsigned char> rgba; // width * height * 4, the first row is the top one
	};

	struct thumbnail_decoded
	{
		std::optional<thumbnail_pixels> pixels;
		std::string error; // why there are none
	};

	// The picture `data` (width x height x channels, 1..4 channels, rows from the bottom when `bottom_up`) made into
	// RGBA8 with the longest side at most `max_side` (a box filter; never larger than the picture), rows from the top.
	// nullopt for a picture with a zero side, 0 or more than 4 channels, or a bad `max_side`.
	std::optional<thumbnail_pixels> downscale_to_thumbnail(const unsigned char* data, int width, int height, int channels,
		bool bottom_up, int max_side = k_thumbnail_side);

	// The picture file `relative` below `root` as a thumbnail: read below the root without following a link (at most
	// k_thumbnail_max_file_bytes), its header checked against the limits above, then decoded. Safe on a worker thread.
	thumbnail_decoded decode_thumbnail_file(const std::filesystem::path& root, const std::filesystem::path& relative);
	// The same; `read_stamp` gets the stamp of the file as the read saw it (unchanged while it was read), for a cache key.
	thumbnail_decoded decode_thumbnail_file(const std::filesystem::path& root, const std::filesystem::path& relative,
		std::optional<platform::beneath::file_stamp>& read_stamp);

	// Whether the picture decoder reads the file by its name (png, jpg, jpeg, bmp, tga, hdr), without case.
	bool thumbnail_decodable(std::string_view path);
}
