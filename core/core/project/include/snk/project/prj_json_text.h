#pragma once
#include <cstdint>
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

	inline constexpr std::uint64_t k_project_file_max_bytes = 16u * 1024u * 1024u;

	struct project_file_text
	{
		bool exists = false;
		std::string text;
		// Set when the file cannot be used: then `exists` says whether something is at its name.
		std::string error;
	};

	// Reads <project_root>/<relative> the way write_project_file reaches it (#526, #527): the root by its path, every
	// folder below it and the file opened without following a link. Nothing at the name (or a folder of `relative`
	// missing): exists = false and no error. A link, a junction, a folder or a FIFO at the name, a link at a folder,
	// or a file over `max_bytes`: exists = true with an error, nothing is read.
	SNK_API project_file_text read_project_file(const std::filesystem::path& project_root, const std::filesystem::path& relative,
		std::uint64_t max_bytes = k_project_file_max_bytes);

	// A file of a folder of the project as the (root, relative) pair the two functions above take: the folder's parent
	// is the root, so the folder and the file are opened without following a link. E.g. <project>/code/X.json ->
	// (<project>, code/X.json).
	struct project_file_location
	{
		std::filesystem::path root;
		std::filesystem::path relative;
	};
	SNK_API project_file_location project_file_location_of(const std::filesystem::path& file);

	// How a file is written: write_text_atomically, or a fake in tests. Returns the error, nullopt on success.
	using text_writer = std::function<std::optional<std::string>(const std::filesystem::path& file, std::string_view text)>;
}
