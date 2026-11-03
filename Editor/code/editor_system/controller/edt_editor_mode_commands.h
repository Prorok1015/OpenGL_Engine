#pragma once
#include <snk/command/cmd_policy.h>
#include <snk/editor/edt_registration.h>
#include <functional>
#include <string_view>
#include <vector>

namespace edt
{
	class action_registry;
	class editor_mode_controller;
	class play_session;

	struct editor_mode_commands_deps
	{
		// Null result: there is none (the editor is going away), the command fails. Null function: never one.
		std::function<editor_mode_controller*()> modes{};
		std::function<play_session*()> play{};
		// A person's gesture is in progress: an agent's switch that would start Play gets `busy` (as play.start).
		std::function<bool()> person_is_editing{};
		// The agent policy's mode for the command with this id (play.start, play.resume, play.pause). An agent's switch
		// that would make that command's change is refused unless it is `allow`. Null: no agent policy, allow.
		std::function<cmd::policy_mode(std::string_view command_id)> agent_mode_of{};
	};

	// editor.mode.toggle{mode?: "editor"|"game"} (#294): F11 and View > Game Mode for a person, a tool of agent category
	// `play` for an agent; without `mode` it switches to the other layout. Returns {mode, play_state}. A refused switch
	// (Play cannot start) is `failed` with the reason. needs_display: there is no layout to switch in a headless editor.
	[[nodiscard]] std::vector<registration> register_editor_mode_commands(action_registry& actions, editor_mode_commands_deps deps);
}
