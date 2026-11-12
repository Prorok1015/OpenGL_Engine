#pragma once
#include <snk/common/snk_api.h>
#include <filesystem>
#include <string>
#include <string_view>

// Where a process writes its log files (#713). Never the folder the process was started from: that is whatever the person's
// shell was in, and for an editor started inside a project it put a logs/ folder into the project.
namespace core {
	// The folder a process without a project keeps its logs in: %LOCALAPPDATA%\Snake on Windows, $XDG_STATE_HOME/snake (an
	// absolute path only), else $HOME/.local/state/snake elsewhere. Empty when none of them is set. Not the editor's user
	// folder: a host without an editor logs there too.
	SNK_API std::filesystem::path user_log_root_for(bool windows, std::string_view local_app_data, std::string_view xdg_state_home, std::string_view home);
	// user_log_root_for with this process's environment.
	SNK_API std::filesystem::path user_log_root();

	// `configured` (a `logger/file` or `logger/category_file/*` value) under `root`: an absolute path stays as it is, a relative
	// one is taken from `root`, never from the current folder. Empty when `configured` or `root` is empty, or when a relative
	// path leaves `root` (".." out of it).
	SNK_API std::filesystem::path resolve_log_path(std::string_view configured, const std::filesystem::path& root);

	// Why `file` (a path under `trusted_root`) may not be written: a folder between the two, or the file itself, is a link or
	// is not a folder, so the write would land elsewhere (a project's .cache linked out of the project). Empty when it may.
	// Folders that do not exist yet are fine (the logger makes them). `trusted_root` empty, or a file not under it: empty.
	SNK_API std::string log_path_problem(const std::filesystem::path& trusted_root, const std::filesystem::path& file);
}
