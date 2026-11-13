#pragma once
#include <snk/common/snk_api.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Where a shipped game (#593, res/read_only_project) keeps what it writes: its project folder is read-only.
namespace core::res {
	// `name` (UTF-8) as one folder name on every platform: UTF-8 letters and [A-Za-z0-9 _.-] stay, any other character
	// becomes '_'; at most 64 bytes, cut on a character boundary; no trailing dots or spaces; a reserved Windows device
	// name (CON, NUL, COM1, "aux.txt"...) gets a '_' appended; "game" when nothing is left.
	SNK_API std::string safe_folder_name(std::string_view name);
	// user:// of the player: %LOCALAPPDATA%\<name> on Windows; $XDG_DATA_HOME/<name> elsewhere, $HOME/.local/share/<name>
	// when that is not set (or not absolute). Empty when none of them is set.
	SNK_API std::filesystem::path player_data_dir(std::string_view project_name);

	// Where the scratch folders of the runs are made: $XDG_RUNTIME_DIR when it is a folder of this user that nobody else
	// can write into (POSIX), else the OS temp folder. Empty (and `error` set) when there is none.
	SNK_API std::filesystem::path scratch_base_dir(std::string& error);

	struct scratch_folder_result
	{
		std::filesystem::path path;
		std::string error;
	};
	// memory:// of one run: a new folder <base>/snk-<name>-<pid>-<random> that this call made itself (POSIX: mkdir 0700,
	// then checked to be a directory of this user, not a link, with mode 0700; Windows: a fresh folder, never an existing
	// one). Nothing planted in a shared temp folder is reused. `path` empty and `error` set when it cannot be made.
	SNK_API scratch_folder_result make_scratch_dir(std::string_view project_name);
	// In the app_data_storage while a read-only project is mounted: memory:// of this run, and the copies of its game
	// modules (core_project_modules). resource_term removes it.
	struct run_scratch_folder
	{
		explicit run_scratch_folder(std::filesystem::path folder) : path(std::move(folder)) {}
		std::filesystem::path path;
	};
	// Removes the scratch folders of this game whose process is gone (a run that crashed). Only a real directory (not a
	// link or a junction) is removed; on POSIX also only one owned by this user with mode 0700, so a folder someone else
	// put in a shared temp folder is never walked. Returns how many.
	SNK_API std::size_t remove_stale_scratch_dirs(std::string_view project_name);
}
