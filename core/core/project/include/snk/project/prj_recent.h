#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

namespace prj
{
	inline constexpr std::size_t k_max_recent_projects = 5;

	// File > Recent Projects (EPIC-42 #158): manifest files, most recent first, no two naming the same file, at most
	// k_max_recent_projects. Stored by the editor as edt://settings/recent_projects.json.
	using recent_projects = std::vector<std::filesystem::path>;

	// `manifest_file` moves to the front; an older entry for the same file (another spelling of its path included) goes.
	SNK_API recent_projects remember_project(recent_projects list, const std::filesystem::path& manifest_file);

	// A file that is not this format (broken, hand-edited, from a newer editor) gives an empty list: Recent is a
	// convenience and never stops the editor. Entries that are not absolute paths are dropped, the rest is cut to
	// k_max_recent_projects without duplicates.
	SNK_API recent_projects parse_recent_projects(std::string_view json);
	SNK_API std::string serialize_recent_projects(const recent_projects& list);
}
