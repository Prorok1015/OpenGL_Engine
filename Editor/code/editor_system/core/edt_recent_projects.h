#pragma once
#include <filesystem>
#include <optional>
#include <snk/project/prj_recent.h>
#include <snk/resource/res_system.h>

namespace edt {

	// File > Recent Projects (EPIC-42 #158), kept per user in edt://settings/recent_projects.json.

	// The stored list; empty on the first run or when the file is broken. Main thread only.
	prj::recent_projects read_recent_projects(res::resource_system& res);

	// Moves `manifest_file` (the project this editor opened) to the front of the stored list and writes it back. Returns
	// the list either way; a failed write is logged and only costs the change.
	prj::recent_projects remember_recent_project(res::resource_system& res, const std::filesystem::path& manifest_file);

	// What the File menu lists at startup: the stored list with `open_manifest` remembered first (when a project is
	// open), then without it, since reopening the project already open is not a choice.
	prj::recent_projects recent_projects_besides(res::resource_system& res,
		const std::optional<std::filesystem::path>& open_manifest);
}
