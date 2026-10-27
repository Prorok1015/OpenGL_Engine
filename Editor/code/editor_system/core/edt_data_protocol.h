#pragma once
#include <filesystem>
#include <snk/resource/res_system.h>

namespace edt {

	inline constexpr std::string_view DATA_PROTOCOL = "edt";

	// Mounts `data_path` as the edt:// protocol (editor-only resources: icons, templates, settings).
	// A missing directory is not fatal: the protocol stays registered and every edt:// load fails with an error.
	// Returns whether the directory exists.
	bool register_data_protocol(res::resource_system& res, const std::filesystem::path& data_path);
}
