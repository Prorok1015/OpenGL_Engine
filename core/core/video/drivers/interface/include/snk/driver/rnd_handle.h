#pragma once
#include <cstdint>

namespace rnd::driver
{
	// Typed index into a driver's table. Index 0 is null; `gen` tells a slot's reuses apart.
	template <class Tag>
	struct handle
	{
		std::uint32_t index = 0;
		std::uint32_t gen = 0;

		constexpr explicit operator bool() const noexcept { return index != 0; }
		friend constexpr bool operator==(const handle&, const handle&) = default;
	};
}
