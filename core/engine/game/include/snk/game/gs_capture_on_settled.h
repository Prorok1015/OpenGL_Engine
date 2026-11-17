#pragma once
#include <snk/application/app_frame_stage.h>
#include <snk/common/snk_api.h>

namespace gs
{
	// Automation for CI and QA: with `capture/on_settled <file.png>` set, the run takes one `viewport.capture`
	// through the process command registry `capture/delay_frames` (default 30) frames after the deterministic scene
	// settled (app::frame_control::settled_frame) and writes the PNG to the file, then logs
	// "captured viewport '<id>' ... to <file>". `capture/viewport` picks the view (default: the first one).
	// Does nothing when the key is empty. Needs app::frame_control and cmd::command_registry in the storage when
	// a frame runs.
	SNK_API void register_capture_on_settled(app::frame_stage_registry& registry);
}
