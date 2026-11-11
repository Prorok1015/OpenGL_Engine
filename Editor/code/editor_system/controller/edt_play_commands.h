#pragma once
#include <snk/application/app_frame_commands.h>
#include <snk/application/app_frame_control.h>
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <functional>
#include <vector>

namespace edt
{
	class action_registry;
	class dialog_service;
	class play_session;

	struct play_commands_deps
	{
		// The session, or null when there is none (the editor is going away): the commands then fail, play.status
		// reads `stopped`. Null function: never a session.
		std::function<play_session*()> play{};
		// A person's gesture is in progress: an agent's play.start gets `busy` (as scene.*). Null: never.
		std::function<bool()> person_is_editing{};
		// Asks the person whether to play past the errors of the check before Play (#597). Null: the menu only refuses.
		dialog_service* dialogs = nullptr;
		// Shows the Problems panel ("Show problems" of that question). Null: the button does nothing.
		std::function<void()> show_problems{};
	};

	// The Play menu as commands for people and agents alike (US-63-C, #491), all of agent category `play`:
	// play.start (menu Play/Resume, Ctrl+P; while paused it resumes), play.pause, play.step{n?} (menu Step: one frame;
	// only while paused, while playing it fails "pause first"), play.stop, and without a menu item play.resume and
	// play.status (read-only). Every one returns the status {state, frame, worlds}: `frame` counts the frames the set
	// "play" simulated since Start (context_set::frames_run), `worlds` its worlds. A call of an agent origin changes the
	// state with play_reason::agent, any other with user. Menu items are enabled as before (Play while a person edits:
	// no; Step: only paused); an agent's call is judged by the command alone and gets the reason of a refusal.
	// play.start first runs the check before Play (#597): with an error, a call that is not a person's is `failed` and
	// a person's is `needs_input`, both with `{problems: [{severity, code, message, world, path?, json_path?}]}`; the
	// menu item (activate) then asks "Play anyway / Show problems / Cancel" (headless: cancelled, logged). Only that
	// answer lets Play past the errors, once; an agent has no way around them.
	[[nodiscard]] std::vector<registration> register_play_commands(action_registry& actions, play_commands_deps deps);

	struct play_frame_commands
	{
		app::frame_command_registrations commands{};
		// Releases the time hold of app.advance whenever Play stops (any path: agent, person, level or module change):
		// app.resume is disabled outside Play, so nothing else could release it then.
		connection release_on_stop{};
	};

	// Drops `core` (app.advance / app.resume as core_module registered them: no category) and registers them again into
	// `commands` with category `play`, enabled only while `play_active` says so: outside Play an agent's app.advance
	// is `disabled` and cannot hold the editor's time. `control` must outlive the result; keep the result no longer
	// than what `play_active` reads.
	[[nodiscard]] play_frame_commands register_play_frame_commands(app::frame_command_registrations& core,
		cmd::command_registry& commands, app::frame_control& control, std::function<bool()> play_active, editor_events& events);
}
