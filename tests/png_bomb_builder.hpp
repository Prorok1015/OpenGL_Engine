#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace test_util::png_bomb {

	// Deflate fills each byte from its low bit; a Huffman code goes in from its high bit.
	struct bit_writer
	{
		std::vector<unsigned char> bytes;
		std::uint32_t pending = 0;
		int count = 0;

		void put(std::uint32_t value, int bits)
		{
			for (int i = 0; i < bits; ++i) {
				pending |= ((value >> i) & 1u) << count;
				if (++count == 8) flush();
			}
		}

		void put_code(std::uint32_t code, int bits)
		{
			for (int i = bits - 1; i >= 0; --i) put((code >> i) & 1u, 1);
		}

		void flush()
		{
			if (count > 0) bytes.push_back(static_cast<unsigned char>(pending));
			pending = 0;
			count = 0;
		}
	};

	inline void put_be32(std::vector<unsigned char>& out, std::uint32_t value)
	{
		for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>((value >> shift) & 0xFFu));
	}

	// A zlib stream of one fixed-Huffman block: `literals` (each below 144, at least one), then the last of them again
	// 258 x `repeats` times, as matches of length 258 at distance 1: 13 bits of file for 258 bytes of output.
	inline std::vector<unsigned char> zlib_stream(const std::vector<unsigned char>& literals, std::size_t repeats)
	{
		bit_writer out;
		out.bytes = { 0x78, 0x01 };
		out.put(1, 1);
		out.put(1, 2);
		std::uint32_t a = 1;
		std::uint32_t b = 0;
		const auto sum = [&a, &b](unsigned char byte) {
			a = (a + byte) % 65521u;
			b = (b + a) % 65521u;
		};
		for (const unsigned char literal : literals) {
			out.put_code(0x30u + literal, 8);
			sum(literal);
		}
		for (std::size_t i = 0; i < repeats; ++i) {
			out.put_code(0xC5u, 8);
			out.put_code(0, 5);
			for (int n = 0; n < 258; ++n) sum(literals.back());
		}
		out.put_code(0, 7);
		out.flush();
		put_be32(out.bytes, (b << 16) | a);
		return out.bytes;
	}

	inline std::uint32_t crc32(const unsigned char* data, std::size_t size)
	{
		std::uint32_t crc = 0xFFFFFFFFu;
		for (std::size_t i = 0; i < size; ++i) {
			crc ^= data[i];
			for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
		return ~crc;
	}

	inline void put_chunk(std::vector<unsigned char>& out, std::string_view type, const std::vector<unsigned char>& data)
	{
		put_be32(out, static_cast<std::uint32_t>(data.size()));
		const std::size_t start = out.size();
		out.insert(out.end(), type.begin(), type.end());
		out.insert(out.end(), data.begin(), data.end());
		put_be32(out, crc32(out.data() + start, out.size() - start));
	}

	// A whole RGBA PNG whose header says width x height and whose IDAT is `idat`.
	inline std::vector<std::byte> png_rgba(std::uint32_t width, std::uint32_t height, const std::vector<unsigned char>& idat)
	{
		std::vector<unsigned char> bytes{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		std::vector<unsigned char> ihdr;
		put_be32(ihdr, width);
		put_be32(ihdr, height);
		for (const unsigned char value : { 8, 6, 0, 0, 0 }) ihdr.push_back(value);
		put_chunk(bytes, "IHDR", ihdr);
		put_chunk(bytes, "IDAT", idat);
		put_chunk(bytes, "IEND", {});
		std::vector<std::byte> out(bytes.size());
		std::memcpy(out.data(), bytes.data(), bytes.size());
		return out;
	}

}
