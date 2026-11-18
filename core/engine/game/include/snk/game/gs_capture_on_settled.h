#pragma once
#include <snk/application/app_frame_stage.h>
#include <snk/common/snk_api.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace gs
{
	// Automation for CI and QA: with `capture/on_settled <file.png>` set, the run takes one `viewport.capture`
	// through the process command registry `capture/delay_frames` (default 30) frames after the deterministic scene
	// settled (app::frame_control::settled_frame) and writes the PNG to the file, then logs
	// "captured viewport '<id>' ... to <file>". `capture/viewport` picks the view (default: the first one).
	// Does nothing when the key is empty. Needs app::frame_control and cmd::command_registry in the storage when
	// a frame runs.
	SNK_API void register_capture_on_settled(app::frame_stage_registry& registry);

	// The file is an output the launcher of the process names, not project data (#1161): it is written with a temporary
	// file and a rename, never through a link at the file, and never inside `project_root` (project files go through
	// the resource system). An empty root checks nothing. Empty when written, else why not.
	SNK_API std::string write_capture(const std::filesystem::path& file, std::string_view png, const std::filesystem::path& project_root);
}
