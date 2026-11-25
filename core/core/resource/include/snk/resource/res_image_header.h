#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// The size a picture file says it has, read from its header alone (#1206): the model import (in the decoder process,
// without the image library) and the editor's check of the decoder's answer both judge an embedded picture by it before
// anything decodes it. Never decodes, never allocates; a header it cannot read is nullopt.
namespace res::image_header
{
	enum class picture_format { png, jpeg, bmp, tga };

	struct picture_size
	{
		picture_format format;
		std::uint32_t width;
		std::uint32_t height;
	};

	namespace detail
	{
		inline std::uint32_t byte_at(std::span<const std::byte> bytes, std::size_t at) { return std::to_integer<std::uint32_t>(bytes[at]); }
		inline std::uint32_t be16(std::span<const std::byte> b, std::size_t at) { return (byte_at(b, at) << 8) | byte_at(b, at + 1); }
		inline std::uint32_t be32(std::span<const std::byte> b, std::size_t at) { return (be16(b, at) << 16) | be16(b, at + 2); }
		inline std::uint32_t le16(std::span<const std::byte> b, std::size_t at) { return byte_at(b, at) | (byte_at(b, at + 1) << 8); }
		inline std::uint32_t le32(std::span<const std::byte> b, std::size_t at) { return le16(b, at) | (le16(b, at + 2) << 16); }

		inline std::optional<picture_size> sized(picture_format format, std::uint32_t width, std::uint32_t height)
		{
			if (width == 0 || height == 0) return std::nullopt;
			return picture_size{ format, width, height };
		}

		inline std::optional<picture_size> png(std::span<const std::byte> b)
		{
			constexpr std::uint32_t signature[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
			if (b.size() < 24) return std::nullopt;
			for (std::size_t i = 0; i < 8; ++i) {
				if (byte_at(b, i) != signature[i]) return std::nullopt;
			}
			// The first chunk is IHDR, 13 bytes long.
			if (be32(b, 8) != 13 || byte_at(b, 12) != 'I' || byte_at(b, 13) != 'H' || byte_at(b, 14) != 'D' || byte_at(b, 15) != 'R') {
				return std::nullopt;
			}
			return sized(picture_format::png, be32(b, 16), be32(b, 20));
		}

		inline std::optional<picture_size> jpeg(std::span<const std::byte> b)
		{
			if (b.size() < 4 || byte_at(b, 0) != 0xFF || byte_at(b, 1) != 0xD8 || byte_at(b, 2) != 0xFF) return std::nullopt;
			std::size_t at = 2;
			while (at + 1 < b.size()) {
				if (byte_at(b, at) != 0xFF) return std::nullopt;
				std::uint32_t marker = byte_at(b, at + 1);
				if (marker == 0xFF) {	// a fill byte
					++at;
					continue;
				}
				at += 2;
				if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;	// no length
				if (marker == 0xD9 || marker == 0xDA) return std::nullopt;	// the end, or the scan, before a frame header
				if (at + 2 > b.size()) return std::nullopt;
				const std::uint32_t length = be16(b, at);
				if (length < 2) return std::nullopt;
				// SOF0..SOF15 but DHT (C4), JPG (C8) and DAC (CC): precision, height, width.
				if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
					if (length < 7 || at + 7 > b.size()) return std::nullopt;
					return sized(picture_format::jpeg, be16(b, at + 5), be16(b, at + 3));
				}
				at += length;
			}
			return std::nullopt;
		}

		inline std::optional<picture_size> bmp(std::span<const std::byte> b)
		{
			if (b.size() < 26 || byte_at(b, 0) != 'B' || byte_at(b, 1) != 'M') return std::nullopt;
			const std::uint32_t info_size = le32(b, 14);
			if (info_size == 12) return sized(picture_format::bmp, le16(b, 18), le16(b, 20));	// BITMAPCOREHEADER
			if (info_size < 40) return std::nullopt;
			const auto width = static_cast<std::int32_t>(le32(b, 18));
			const auto height = static_cast<std::int32_t>(le32(b, 22));
			// A negative height is a picture stored top row first; the lowest int has no positive counterpart.
			if (width <= 0 || height == INT32_MIN) return std::nullopt;
			return sized(picture_format::bmp, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height < 0 ? -height : height));
		}

		// TGA has no signature: a header is taken only when every field the decoder reads is one it decodes.
		inline std::optional<picture_size> tga(std::span<const std::byte> b)
		{
			if (b.size() < 18) return std::nullopt;
			const std::uint32_t colour_map = byte_at(b, 1);
			const std::uint32_t type = byte_at(b, 2);
			const std::uint32_t depth = byte_at(b, 16);
			if (colour_map > 1) return std::nullopt;
			const bool mapped = type == 1 || type == 9;
			const bool known_type = mapped || type == 2 || type == 3 || type == 10 || type == 11;
			if (!known_type || mapped != (colour_map == 1)) return std::nullopt;
			// The depth stb_image decodes for each kind: colour-map indices and grey of 8 or 16 bits, true colour of 15 to 32.
			const bool grey = type == 3 || type == 11;
			const bool depth_known = (mapped || grey) ? depth == 8 || depth == 16 : depth == 15 || depth == 16 || depth == 24 || depth == 32;
			if (!depth_known) return std::nullopt;
			return sized(picture_format::tga, le16(b, 12), le16(b, 14));
		}
	}

	// PNG, JPEG and BMP by their signature, then TGA by a header it would decode.
	inline std::optional<picture_size> read_picture_size(std::span<const std::byte> bytes)
	{
		if (auto size = detail::png(bytes)) return size;
		if (auto size = detail::jpeg(bytes)) return size;
		if (auto size = detail::bmp(bytes)) return size;
		return detail::tga(bytes);
	}
}
