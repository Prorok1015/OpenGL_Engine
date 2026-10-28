#pragma once
#include <filesystem>
#include <string_view>
#include <snk/resource/res_system.h>

namespace edt {

	inline constexpr std::string_view DATA_PROTOCOL = "edt";

	// Mounts `data_path` (Editor/data: icons, templates; shipped with the editor) as the read-only edt:// protocol.
	// A missing directory is not fatal: the protocol stays mounted and its loads fail with an error.
	// Returns whether the directory exists.
	bool register_data_protocol(res::resource_system& res, const std::filesystem::path& data_path);
}
