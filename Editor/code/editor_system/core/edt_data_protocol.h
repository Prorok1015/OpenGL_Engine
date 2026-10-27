#pragma once
#include <filesystem>
#include <string_view>
#include <snk/resource/res_system.h>

namespace edt {

	inline constexpr std::string_view DATA_PROTOCOL = "edt";

	// %APPDATA%/Snake/Editor on Windows, $XDG_DATA_HOME/snake/editor (default ~/.local/share/snake/editor) elsewhere.
	// Falls back to a folder in the temp directory when the environment names no home.
	std::filesystem::path default_user_data_dir();

	// Mounts edt:// (editor-only resources: icons, templates, settings) over two roots: `data_path` (Editor/data, shipped
	// with the editor, only ever read) under `user_path` (per-user, the only root edt:// writes go to).
	// A missing `data_path` is not fatal: the protocol stays registered and its loads fail with an error.
	// Returns whether `data_path` exists.
	bool register_data_protocol(res::resource_system& res, const std::filesystem::path& data_path, const std::filesystem::path& user_path);
}
