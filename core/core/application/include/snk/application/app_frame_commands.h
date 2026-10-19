#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/application/app_frame_control.h>
#include <snk/common/snk_api.h>

namespace app
{
	// Registers `app.advance{frames, fixed_dt?}` -> `{frame, target_frame}`: grants `frames` frames of simulation
	// time (see frame_control) and holds the time between grants. The frames run after the call returns, one per
	// loop iteration: `frame` is the frame count now, `target_frame` the one the granted frames end at.
	// `fixed_dt` (seconds, > 0) applies from this grant on; default: the fixed dt of the run, else 1/60.
	// `control` must outlive the registration.
	[[nodiscard]] SNK_API cmd::registration register_frame_commands(cmd::command_registry& registry, frame_control& control);
}
