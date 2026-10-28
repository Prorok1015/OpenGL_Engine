#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <snk/resource/res_system.h>

namespace edt {

	// Per-user editor settings live in edt://settings/ (the user root of edt://, never the installed Editor/data):
	// window layout, recent files, preferences. `name` is a path relative to that folder, e.g. "recent_files.json".
	res::tag settings_tag(std::string_view name);

	// Stores `text` as edt://settings/<name> and returns once the file is written (resource_system::store_sync, so a
	// read right after cannot race the io thread). False when the write was refused (a name that leaves settings/, a
	// resource layer that may not write the tag, the edt protocol not registered) or failed, and never throws; the file
	// is then as it was. Main thread: evicts the cached copy so the
	// next read_setting sees the new text.
	bool write_setting(res::resource_system& res, std::string_view name, std::string_view text);

	// nullopt when the file does not exist (the normal first run) or cannot be read. Main thread only.
	std::optional<std::string> read_setting(res::resource_system& res, std::string_view name);
}
