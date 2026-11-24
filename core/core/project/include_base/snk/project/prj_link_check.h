#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

// Base only (include_base/, not installed in the SDK): the check of the project's cache for links (#1241).
namespace prj
{
	// A cache of a few thousand files is the usual size; this many is far past it and is still walked in seconds. More
	// is refused rather than checked in part: a link past the limit would go unseen.
	inline constexpr std::size_t k_link_walk_limit = 200'000;

	// How link_below walks: how far, what it calls the folder in its messages, and what it does not enter.
	struct link_walk_options
	{
		std::size_t max_entries = k_link_walk_limit;
		// Completes "is a link or an unsupported entry (...) in <what>" and "holds more than N entries (it is <what>)".
		std::string what = "the project's cache";
		// A folder this says yes to (its path relative to the walked folder, '/' separated) is checked as an entry like any
		// other, so a link at it is refused, but its contents are not walked. Null: every folder is walked.
		std::function<bool(const std::string& relative)> skip_folder;
	};

	// `.snk/migrations/<id>/backup`: copies of the project's own files, many; a rollback reads them through
	// `prj::read_project_file`, which follows no link, so a link planted there is refused when it is read, not walked at open.
	inline bool is_migration_backup_folder(const std::string& relative)
	{
		constexpr std::string_view head = "migrations/";
		constexpr std::string_view tail = "/backup";
		if (relative.size() <= head.size() + tail.size() || relative.compare(0, head.size(), head) != 0) return false;
		if (relative.compare(relative.size() - tail.size(), tail.size(), tail) != 0) return false;
		const std::string_view id(relative.data() + head.size(), relative.size() - head.size() - tail.size());
		return !id.empty() && id.find('/') == std::string_view::npos;
	}

	// Why `dir` holds a link (a symlink, a junction or any other entry that redirects its name: on Windows by its
	// reparse tag, platform::beneath::link_entry_at, #1295) at any depth, every entry checked without following one;
	// empty when it holds none or does not exist. A walk past `max_entries` entries, or one that cannot read an entry,
	// is a refusal too. Every reason names the entry and tells the person what to do; nothing is removed here.
	SNK_API std::optional<std::string> link_below(const std::filesystem::path& dir, const link_walk_options& options);
	SNK_API std::optional<std::string> link_below(const std::filesystem::path& dir,
		std::size_t max_entries = k_link_walk_limit);

	// A link found below a folder that leads outside the project.
	struct leaving_link
	{
		std::filesystem::path link;
		std::filesystem::path target;
	};

	struct leaving_links
	{
		std::vector<leaving_link> found;
		// False when the walk stopped (more than `max_entries` entries, or an entry that could not be read): there may be
		// more than `found` says.
		bool complete = true;
	};

	// The links below `dir` (any depth, none followed) whose target, resolved as far as it exists, is not inside `root`, or is
	// inside one of `private_dirs` (the project's own `.snk` and `.cache`: a link into them reads or writes what no path of the
	// project's content should reach). Reads nothing but names; changes nothing.
	SNK_API leaving_links links_leaving_project(const std::filesystem::path& dir, const std::filesystem::path& root,
		std::size_t max_entries = k_link_walk_limit, const std::vector<std::filesystem::path>& private_dirs = {});
}
