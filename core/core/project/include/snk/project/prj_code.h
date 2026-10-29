#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

// What code:// (the project's code/ folder, #450) is made of: which files count as code, which folders are never part
// of it, and what a store into it may write. One source of truth for the resolver's write policy, the file watcher,
// the Asset Browser's Code tree and the agent policy (A6c, #449).
namespace prj
{
	inline constexpr std::string_view k_code_protocol = "code";

	enum class code_category
	{
		source,
		header,
		// CMakeLists.txt, *.cmake, CMake*Presets.json: editing one runs code at configure time, so the agent policy
		// treats them apart from sources.
		cmake,
		// Not code: hidden from the tree, refused by a store.
		other,
	};

	// By the last segment of `path` ('/' or '\' separated); case does not matter. Pure, no file system access.
	SNK_API code_category code_category_of(std::string_view path);

	inline bool is_code_file(std::string_view path)
	{
		return code_category_of(path) != code_category::other;
	}

	// Folder names that are never code, anywhere below code/: build output, fetched dependencies, the editor's own
	// and the project's private folders. Any name starting with '.' is ignored too (see is_code_ignored_dir).
	SNK_API std::span<const std::string_view> code_ignored_dir_names();

	// A single folder name (no separators), compared case-insensitively.
	SNK_API bool is_code_ignored_dir(std::string_view name);

	// The write_policy patterns for protocol "code": code files anywhere in code/, except inside an ignored folder
	// (deny patterns, "!code://..."). The policy built from them must fold case (write_policy case_sensitive=false).
	SNK_API std::vector<std::string> code_write_patterns();
}
