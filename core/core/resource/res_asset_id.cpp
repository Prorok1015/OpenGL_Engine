#include <snk/resource/res_asset_id.h>
#include <random>

namespace
{
	constexpr bool is_dash_position(std::size_t i)
	{
		return i == 8 || i == 13 || i == 18 || i == 23;
	}

	int hex_value(char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}
}

std::optional<res::asset_id> res::asset_id_from_text(std::string_view text)
{
	if (text.size() != 36) {
		return std::nullopt;
	}
	asset_id id;
	int digits = 0;
	for (std::size_t i = 0; i < text.size(); ++i) {
		if (is_dash_position(i)) {
			if (text[i] != '-') return std::nullopt;
			continue;
		}
		const int value = hex_value(text[i]);
		if (value < 0) return std::nullopt;
		std::uint64_t& half = digits < 16 ? id.hi : id.lo;
		half = (half << 4) | static_cast<std::uint64_t>(value);
		++digits;
	}
	return id;
}

std::string res::to_text(const asset_id& id)
{
	constexpr const char* hex = "0123456789abcdef";
	std::string text(36, '-');
	int digit = 0;
	for (std::size_t i = 0; i < text.size(); ++i) {
		if (is_dash_position(i)) continue;
		const std::uint64_t half = digit < 16 ? id.hi : id.lo;
		const int shift = 60 - 4 * (digit % 16);
		text[i] = hex[(half >> shift) & 0xF];
		++digit;
	}
	return text;
}

res::asset_id res::generate_asset_id()
{
	// Straight from random_device, not a PRNG seeded by it: one 32-bit seed gives only 2^32 streams, and two machines
	// that drew the same seed would hand out the same ids. Ids are made rarely, so the cost does not matter.
	std::random_device device;
	const auto word = [&device] {
		return (static_cast<std::uint64_t>(device()) << 32) | static_cast<std::uint64_t>(device());
	};
	asset_id id{ word(), word() };
	id.hi = (id.hi & ~0xF000ull) | 0x4000ull;            // version 4: the 13th hex digit
	id.lo = (id.lo & ~(0xCull << 60)) | (0x8ull << 60); // variant 8-b: the 17th hex digit
	return id;
}
