#pragma once
#include <filesystem>
#include <optional>
#include <system_error>
#include <snk/common/snk_api.h>
#include <snk/platform/plt_reparse_tag.h>

// Base only (include_base/, not installed in the SDK): whether an entry of a project is a link, by what the file system
// says of the entry itself (#1295), so a Windows reparse point std::filesystem does not call a symlink or a junction
// is seen too.
namespace platform::beneath {

	// The entry at `path`, opened as itself without following a link: Windows by its attributes and reparse tag
	// (FileAttributeTagInfo, decided by link_entry_of), POSIX by lstat (a symlink is a link). nullopt with `ec` clear:
	// nothing is there. nullopt with `ec` set: something is there that could not be opened or inspected.
	SNK_API std::optional<link_entry> link_entry_at(const std::filesystem::path& path, std::error_code& ec);

	// `path` in the form that reaches an entry past MAX_PATH: on Windows absolute, normalized and prefixed with \\?\
	// (\\?\UNC\ for a share), unchanged when it already is or cannot be made absolute; unchanged elsewhere.
	SNK_API std::filesystem::path long_path(const std::filesystem::path& path);

}
