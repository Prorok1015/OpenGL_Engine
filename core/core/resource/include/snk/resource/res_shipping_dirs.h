#pragma once
#include <snk/common/snk_api.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Where a shipped game (#593, res/read_only_project) keeps what it writes: its project folder is read-only.
namespace core::res {
	// `name` as one folder name on every platform: characters outside [A-Za-z0-9 _.-] become '_', trailing dots and
	// spaces go; "game" when nothing is left.
	SNK_API std::string safe_folder_name(std::string_view name);
	// user:// of the player: %LOCALAPPDATA%\<name> on Windows; $XDG_DATA_HOME/<name> elsewhere, $HOME/.local/share/<name>
	// when that is not set (or not absolute). Empty when none of them is set.
	SNK_API std::filesystem::path player_data_dir(std::string_view project_name);
	// memory:// of one run: <temp>/snk-<name>-<pid>, removed when the run ends (resource_term).
	SNK_API std::filesystem::path scratch_dir_for(std::string_view project_name, std::int64_t pid);
	// Removes <temp>/snk-<name>-<pid> folders whose process is gone (a run that crashed). Returns how many.
	SNK_API std::size_t remove_stale_scratch_dirs(std::string_view project_name);
}
