#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <snk/resource/res_system.h>

namespace edt {

	inline constexpr std::string_view DATA_PROTOCOL = "edt";

	// %APPDATA%/Snake/Editor on Windows, $XDG_DATA_HOME/snake/editor (default ~/.local/share/snake/editor) elsewhere.
	// Falls back to a folder in the temp directory when the environment names no home.
	std::filesystem::path default_user_data_dir();

	// What edt:// may store into: only the per-user settings (see res::write_policy for the pattern syntax). The default
	// of the editor/edt_write_patterns setting, a list in the config.
	std::vector<std::string> default_write_patterns();

	// Mounts edt:// (editor-only resources: icons, templates, settings) over two roots: `data_path` (Editor/data, shipped
	// with the editor, only ever read) under `user_path` (per-user, the only root edt:// writes go to).
	// store() into a path no pattern of `write_patterns` allows is refused; loads are never restricted.
	// A missing `data_path` is not fatal: the protocol stays registered and its loads fail with an error.
	// Returns whether `data_path` exists.
	bool register_data_protocol(res::resource_system& res, const std::filesystem::path& data_path, const std::filesystem::path& user_path,
		const std::vector<std::string>& write_patterns = default_write_patterns());
}
