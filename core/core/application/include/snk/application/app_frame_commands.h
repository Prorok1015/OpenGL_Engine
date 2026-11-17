#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/application/app_frame_control.h>
#include <snk/command/cmd_types.h>
#include <snk/common/snk_api.h>
#include <functional>

namespace app
{
	// Registers `app.advance{frames, fixed_dt?}` -> `{frame, target_frame}`: grants `frames` frames of simulation
	// time (see frame_control) and holds the time between grants. The frames run after the call returns, one per
	// loop iteration: `frame` is the frame count now, `target_frame` the one the granted frames end at.
	// `fixed_dt` (seconds, > 0) applies from this grant on; default: the fixed dt of the run, else 1/60.
	// Also registers `app.resume{}` -> `{frame}`: releases the hold (frame_control::release), so the loop runs on its
	// own time again. Without it an agent that stopped after `app.advance` would leave the editor frozen.
	// `control` must outlive the registrations.
	struct frame_command_registrations
	{
		cmd::registration advance;
		cmd::registration resume;

		bool active() const { return advance.active() && resume.active(); }
	};

	// What a host changes about the two commands. The defaults are those of every host: no category (closed to
	// agents), always enabled. The editor registers them again with category `play`, enabled only during Play.
	struct frame_command_options
	{
		cmd::agent_category category = cmd::agent_category::unset;
		// Null: always enabled.
		std::function<bool()> enabled{};
	};

	[[nodiscard]] SNK_API frame_command_registrations register_frame_commands(cmd::command_registry& registry, frame_control& control,
		frame_command_options options = {});
}
