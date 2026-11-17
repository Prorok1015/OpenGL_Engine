#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <snk/common/snk_api.h>

namespace res
{
	// The identity of an asset (EPIC-57): a UUID read from its .resource sidecar, independent of the asset's path.
	// `hi` holds the first 16 hex digits of the text form, `lo` the last 16.
	struct asset_id
	{
		std::uint64_t hi = 0;
		std::uint64_t lo = 0;

		bool is_nil() const { return hi == 0 && lo == 0; }

		friend bool operator==(const asset_id&, const asset_id&) = default;
		friend auto operator<=>(const asset_id&, const asset_id&) = default;
	};

	// Accepts only the 8-4-4-4-12 form; hex digits in either case.
	SNK_API std::optional<asset_id> asset_id_from_text(std::string_view text);

	// Lowercase 8-4-4-4-12.
	SNK_API std::string to_text(const asset_id& id);

	// Random UUID v4: version 4, variant 8, 9, a or b.
	SNK_API asset_id generate_asset_id();
}

template <>
struct std::hash<res::asset_id>
{
	std::size_t operator()(const res::asset_id& id) const noexcept
	{
		return std::hash<std::uint64_t>{}(id.hi ^ (id.lo * 0x9E3779B97F4A7C15ull));
	}
};
