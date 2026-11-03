#include "edt_editor_mode_commands.h"
#include "edt_editor_mode.h"
#include "edt_play_session.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/string.hpp>
#include <boost/json/string_view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace json = boost::json;

std::vector<edt::registration> edt::register_editor_mode_commands(action_registry& actions, editor_mode_commands_deps deps)
{
	const auto modes = [get = std::move(deps.modes)]() -> editor_mode_controller* { return get ? get() : nullptr; };
	const auto session = [get = std::move(deps.play)]() -> play_session* { return get ? get() : nullptr; };
	// Going to the game from a stopped Play starts it, which locks the document.
	const auto would_start = [modes, session] {
		const editor_mode_controller* controller = modes();
		const play_session* play = session();
		return controller && play && controller->mode() == editor_mode::editor && play->state() == play_state::stopped;
	};

	cmd::command_desc toggle;
	toggle.id = "editor.mode.toggle";
	toggle.summary = "Switch the editor between game mode (the Game view fills the window, Play runs) and editor mode "
		"(the panels; a running Play is paused). mode: the one to switch to, default the other. Going to the game starts a "
		"stopped Play and resumes only a pause this switch made. Returns {mode, play_state}";
	toggle.category = cmd::agent_category::play;
	toggle.mutates = true;
	toggle.needs_display = true;
	toggle.args_schema = json::parse(R"json({
		"type": "object",
		"properties": { "mode": { "type": "string", "enum": ["editor", "game"], "description": "the mode to switch to; absent: the other one" } },
		"additionalProperties": false
	})json");
	toggle.result_schema = json::parse(R"json({
		"type": "object",
		"properties": {
			"mode": { "type": "string", "enum": ["editor", "game"] },
			"play_state": { "type": "string", "enum": ["stopped", "playing", "paused"] }
		},
		"required": ["mode", "play_state"],
		"additionalProperties": false
	})json");
	toggle.enabled = [modes, session] { return modes() != nullptr && session() != nullptr; };
	toggle.invoke = [modes, session, would_start, editing = deps.person_is_editing](const cmd::call& call) {
		editor_mode_controller* controller = modes();
		play_session* play = session();
		if (!controller || !play) return cmd::result::failure(cmd::codes::failed, "the editor mode is not available");

		editor_mode target = controller->mode() == editor_mode::editor ? editor_mode::game : editor_mode::editor;
		if (const json::value* given = call.args.as_object().if_contains("mode")) {
			const json::string* name = given->if_string();
			const std::optional<editor_mode> parsed = name ? editor_mode_of(std::string_view(name->data(), name->size())) : std::nullopt;
			if (!parsed) return cmd::result::failure(cmd::codes::invalid_args, "mode is not \"editor\" or \"game\"");
			target = *parsed;
		}
		const bool agent = call.from.kind == cmd::origin_kind::agent;
		if (agent && target == editor_mode::game && would_start() && editing && editing()) {
			return cmd::result::failure(cmd::codes::busy, "a person is in the middle of an edit; call again when it ends");
		}
		if (auto refused = controller->set_mode(target, agent ? play_reason::agent : play_reason::user)) {
			return cmd::result::failure(cmd::codes::failed, *refused);
		}
		json::object result;
		result["mode"] = json::string_view(editor_mode_name(controller->mode()));
		result["play_state"] = json::string_view(play_state_name(play->state()));
		return cmd::result::success(std::move(result));
	};

	action_ui ui;
	ui.label = "Game Mode";
	ui.menu_path = "View";
	ui.shortcut = "F11";
	// Like Ctrl+P: a key pressed in a text field belongs to the field.
	ui.yields_to_text_input = true;
	ui.checked = [modes] {
		const editor_mode_controller* controller = modes();
		return controller && controller->mode() == editor_mode::game;
	};
	ui.enabled = [would_start, editing = std::move(deps.person_is_editing)] { return !(would_start() && editing && editing()); };

	std::vector<registration> out;
	out.push_back(actions.add(std::move(toggle), std::move(ui)));
	return out;
}
