#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <boost/json/value.hpp>
#include <snk/common/snk_api.h>

namespace prj
{
	// JSON for files people read and edit (the manifest, CMakeUserPresets.json): 4-space indents, `"key": value`,
	// one member or element per line, empty objects and arrays as `{}` / `[]`, object members in their stored order,
	// a trailing newline.
	SNK_API std::string to_indented_json(const boost::json::value& value);

	// Writes `text` to a temporary file next to `file` and renames it over `file`, so a reader never sees half of it.
	// A symbolic link is replaced at its target. The error names the file; nullopt on success.
	SNK_API std::optional<std::string> write_text_atomically(const std::filesystem::path& file, std::string_view text);

	// Writes `text` to <project_root>/<relative> like write_text_atomically, but never through a link below the root
	// (#523): the root is opened by its path (it may itself be a link), then every folder of `relative` is opened from
	// the one above it without following a link and made if missing, and the file is replaced inside the last one
	// (platform::beneath, durable). A link (or a junction) at a folder or at the file is an error and its target stays
	// untouched. `relative` is a chain of plain names, e.g. ".snk/build.json". The error names the file; nullopt on success.
	SNK_API std::optional<std::string> write_project_file(const std::filesystem::path& project_root, const std::filesystem::path& relative,
		std::string_view text);

	// How a file is written: write_text_atomically, or a fake in tests. Returns the error, nullopt on success.
	using text_writer = std::function<std::optional<std::string>(const std::filesystem::path& file, std::string_view text)>;
}
