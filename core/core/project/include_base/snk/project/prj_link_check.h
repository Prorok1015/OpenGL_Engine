#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <snk/common/snk_api.h>

// Base only (include_base/, not installed in the SDK): the check of the project's cache for links (#1241).
namespace prj
{
	// A cache of a few thousand files is the usual size; this many is far past it and is still walked in seconds. More
	// is refused rather than checked in part: a link past the limit would go unseen.
	inline constexpr std::size_t k_link_walk_limit = 200'000;

	// Why `dir` holds a link (a symlink, a junction or any other entry that redirects its name: on Windows by its
	// reparse tag, platform::beneath::link_entry_at, #1295) at any depth, every entry checked without following one;
	// empty when it holds none or does not exist. A walk past `max_entries` entries, or one that cannot read an entry,
	// is a refusal too. Every reason names the entry and tells the person what to do; nothing is removed here.
	SNK_API std::optional<std::string> link_below(const std::filesystem::path& dir,
		std::size_t max_entries = k_link_walk_limit);
}
