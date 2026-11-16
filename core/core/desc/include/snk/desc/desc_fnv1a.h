#pragma once
#include <cstdint>
#include <string_view>

namespace desc
{
	// 64-bit FNV-1a: stable across platforms and STLs (std::hash is not), so it can end up in saved data (node ids, type ids).
	// A header of its own (#1077): the model import derives node ids without desc_system.
	constexpr std::uint64_t fnv1a_64(std::string_view text) noexcept
	{
		std::uint64_t h = 14695981039346656037ull;
		for (unsigned char c : text) {
			h ^= c;
			h *= 1099511628211ull;
		}
		return h;
	}
}
