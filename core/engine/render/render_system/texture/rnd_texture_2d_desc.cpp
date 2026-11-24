#include "rnd_texture_2d_desc.h"
#include <snk/command/cmd_base64.h>
#include <snk/common/text_escape.h>
#include <snk/model_codec/res_reference.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_diagnostics.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using pixel_format = rnd::driver::texture_header::TYPE;

	[[noreturn]] void refuse(std::string_view json_path, const std::string& reason)
	{
		throw res::load_error(res::diagnostic_kind::load, json_path, reason);
	}

	// The formats a decoded picture is uploaded in: 1, 3 or 4 bytes a pixel.
	struct format_name
	{
		pixel_format format;
		std::string_view name;
		std::size_t bytes_per_pixel;
	};

	constexpr format_name k_formats[] = {
		{ pixel_format::R8, "R8", 1 },
		{ pixel_format::RGB8, "RGB8", 3 },
		{ pixel_format::RGBA8, "RGBA8", 4 },
	};

	const format_name& format_of(pixel_format format)
	{
		for (const auto& entry : k_formats) {
			if (entry.format == format) return entry;
		}
		return k_formats[2];
	}

	const json::value& field(const json::object& pixels, std::string_view key)
	{
		const json::value* value = pixels.if_contains(key);
		if (!value) refuse(std::format("/pixels/{}", key), std::format("inline pixels need '{}'", key));
		return *value;
	}

	uint32_t side(const json::object& pixels, std::string_view key)
	{
		const json::value& value = field(pixels, key);
		const std::string path = std::format("/pixels/{}", key);
		if (!value.is_int64() && !value.is_uint64()) refuse(path, std::format("'{}' is not a whole number", key));
		const bool in_range = value.is_int64()
			? value.as_int64() >= 1 && value.as_int64() <= rnd::texture_2d_desc::k_max_inline_side
			: value.as_uint64() <= rnd::texture_2d_desc::k_max_inline_side;
		if (!in_range) {
			refuse(path, std::format("'{}' is {}: 1 to {}", key, json::serialize(value), rnd::texture_2d_desc::k_max_inline_side));
		}
		return static_cast<uint32_t>(value.is_int64() ? value.as_int64() : value.as_uint64());
	}

	rnd::inline_pixels read_pixels(const json::value& value)
	{
		if (!value.is_object()) refuse("/pixels", "inline pixels are an object {width, height, format, base64}");
		const json::object& pixels = value.as_object();
		rnd::inline_pixels out;
		out.width = side(pixels, "width");
		out.height = side(pixels, "height");

		const json::value& format = field(pixels, "format");
		if (!format.is_string()) refuse("/pixels/format", "'format' is not a string");
		const format_name* found = nullptr;
		for (const auto& entry : k_formats) {
			if (format.as_string() == entry.name) found = &entry;
		}
		if (!found) {
			refuse("/pixels/format", std::format("unknown inline pixel format '{}': R8, RGB8 or RGBA8",
				text_escape::escaped_text(format.as_string(), 64)));
		}
		out.format = found->format;

		// Both sides are at most 4096, so the product of three of them cannot overflow.
		const std::size_t expected = static_cast<std::size_t>(out.width) * out.height * found->bytes_per_pixel;
		if (expected > rnd::texture_2d_desc::k_max_inline_bytes) {
			refuse("/pixels", std::format("{}x{} {} is {} bytes: inline pixels are at most {} bytes, use a picture file", out.width,
				out.height, found->name, expected, rnd::texture_2d_desc::k_max_inline_bytes));
		}

		const json::value& text = field(pixels, "base64");
		if (!text.is_string()) refuse("/pixels/base64", "'base64' is not a string");
		const std::string_view encoded = text.as_string();
		// Checked before decoding: a long string is refused without allocating for it.
		const std::size_t encoded_size = (expected + 2) / 3 * 4;
		if (encoded.size() != encoded_size) {
			refuse("/pixels/base64", std::format("'base64' has {} characters: {}x{} {} needs {} bytes, {} characters", encoded.size(),
				out.width, out.height, found->name, expected, encoded_size));
		}
		std::optional<std::vector<std::byte>> decoded = cmd::base64::decode(encoded);
		// Only the canonical text of the bytes: unused bits left nonzero would not survive a save.
		if (!decoded || decoded->size() != expected || cmd::base64::encode(*decoded) != encoded) {
			refuse("/pixels/base64", "'base64' is not padded standard base64 (RFC 4648) of the pixels");
		}
		out.bytes.resize(expected);
		std::memcpy(out.bytes.data(), decoded->data(), expected);
		return out;
	}

	json::object write_pixels(const rnd::inline_pixels& pixels)
	{
		const auto bytes = std::as_bytes(std::span{ pixels.bytes });
		return json::object{
			{ "width", pixels.width },
			{ "height", pixels.height },
			{ "format", format_of(pixels.format).name },
			{ "base64", cmd::base64::encode(bytes) },
		};
	}
}

void rnd::texture_2d_desc::deserialize(desc::desc_system& system, const json::object& resource)
{
	texture_desc::deserialize(system, resource);

	// A null is what a file over a `__parent` writes for a key the parent has and it has not (merge patch, #821).
	const json::value* pixels = resource.if_contains("pixels");
	const json::value* data = resource.if_contains("data");
	const bool gives_pixels = pixels && !pixels->is_null();
	if (gives_pixels && data && !data->is_null()) {
		refuse("/pixels", "a texture gives either 'data' (a picture) or 'pixels', not both");
	}
	// What the file names replaces what its parent gave: `data` a parent's pixels, `pixels` a parent's picture.
	if (gives_pixels) {
		m_pixels = read_pixels(*pixels);
		picture_handle = {};
		return;
	}
	if (pixels) m_pixels.reset();
	// A reference in either form a file writes it: a path, or {guid, path} (#920).
	if (data && res::reference_from_json(*data)) {
		m_pixels.reset();
		picture_handle = system.get_field_desc<res::picture_resource>(*this, *data);
	} else if (picture_handle.is_valid() && !picture_handle.has_error() && picture_handle.get_tag().is_valid()) {
		// A cached parent may still hold the picture from before a file change. A failed or refused one (#697: no tag) stays.
		picture_handle = system.get_field_desc<res::picture_resource>(*this, json::value_from(picture_handle.get_tag()));
	}
}

void rnd::texture_2d_desc::serialize(json::object& resource) const
{
	texture_desc::serialize(resource);

	if (m_pixels) {
		resource["pixels"] = write_pixels(*m_pixels);
		return;
	}
	picture_handle.wait();
	if (picture_handle.is_ready()) {
		resource["data"] = json::value_from(picture_handle.get_tag());
	}
}
