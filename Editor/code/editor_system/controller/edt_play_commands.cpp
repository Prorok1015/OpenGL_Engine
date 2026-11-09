#include "edt_play_commands.h"
#include "edt_play_session.h"
#include <snk/command/cmd_json_schema.h>
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/string_view.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace json = boost::json;

namespace {

	constexpr std::int64_t k_max_step_frames = 600;

	json::value status_schema()
	{
		return json::parse(R"json({
			"type": "object",
			"properties": {
				"state": { "type": "string", "enum": ["stopped", "playing", "paused"] },
				"frame": { "type": "integer", "minimum": 0, "description": "frames the play set simulated since Start (Play and Step frames; 0 when stopped); not app.advance's frame" },
				"worlds": { "type": "array", "items": { "type": "string" } }
			},
			"required": ["state", "frame", "worlds"],
			"additionalProperties": false
		})json");
	}

	json::object status_of(const edt::play_session* play)
	{
		json::array worlds;
		if (play) {
			for (const std::string& name : play->world_names()) worlds.emplace_back(json::string_view(name.data(), name.size()));
		}
		json::object status;
		status["state"] = json::string_view(edt::play_state_name(play ? play->state() : edt::play_state::stopped));
		status["frame"] = static_cast<std::int64_t>(play ? play->frames_run() : std::uint64_t{ 0 });
		status["worlds"] = std::move(worlds);
		return status;
	}

	edt::play_reason reason_of(const cmd::call& call)
	{
		return call.from.kind == cmd::origin_kind::agent ? edt::play_reason::agent : edt::play_reason::user;
	}

	cmd::command_desc play_command(std::string id, std::string summary)
	{
		cmd::command_desc command;
		command.id = std::move(id);
		command.summary = std::move(summary);
		command.category = cmd::agent_category::play;
		command.mutates = true;
		command.result_schema = status_schema();
		return command;
	}

	edt::action_ui menu_item(std::string label)
	{
		edt::action_ui ui;
		ui.label = std::move(label);
		ui.menu_path = "Play";
		return ui;
	}

}

std::vector<edt::registration> edt::register_play_commands(action_registry& actions, play_commands_deps deps)
{
	const auto session = [play = std::move(deps.play)]() -> play_session* { return play ? play() : nullptr; };
	const auto unavailable = [] { return cmd::result::failure(cmd::codes::failed, "Play is not available"); };
	const auto reloading = [] { return cmd::result::failure(cmd::codes::busy, "module reload in progress"); };
	std::vector<registration> out;
	out.reserve(6);

	cmd::command_desc start = play_command("play.start",
		"Start Play of the scene as it is edited (saved or not); while paused, resume it. Returns the play status "
		"(frame: frames the play set simulated since Start, not app.advance's frame)");
	start.enabled = [session] { return session() != nullptr; };
	start.invoke = [session, unavailable, reloading, editing = deps.person_is_editing](const cmd::call& call) {
		play_session* play = session();
		if (!play) return unavailable();
		if (play->reloading()) return reloading();
		// Resuming touches no document, as play.resume.
		if (play->state() == play_state::paused) {
			play->resume(reason_of(call));
			return cmd::result::success(status_of(play));
		}
		// Start locks the document: a value the person has not committed yet would be lost.
		if (call.from.kind == cmd::origin_kind::agent && editing && editing()) {
			return cmd::result::failure(cmd::codes::busy, "a person is in the middle of an edit; call again when it ends");
		}
		if (auto refused = play->start_refusal()) return cmd::result::failure(cmd::codes::failed, *refused);
		if (!play->start(reason_of(call))) return cmd::result::failure(cmd::codes::failed, "Play did not start: see the log");
		return cmd::result::success(status_of(play));
	};
	action_ui start_ui = menu_item("Play");
	start_ui.shortcut = "Ctrl+P";
	start_ui.dynamic_label = [session] {
		const play_session* play = session();
		return std::string{ play && play->state() == play_state::paused ? "Resume" : "Play" };
	};
	// Ctrl+P in a text field (a rename in the hierarchy) is typing.
	start_ui.yields_to_text_input = true;
	start_ui.enabled = [session, editing = deps.person_is_editing] {
		const play_session* play = session();
		return play && !play->reloading() && (play->state() == play_state::paused
			|| (!play->active() && !play->start_refusal() && !(editing && editing())));
	};
	out.push_back(actions.add(std::move(start), std::move(start_ui)));

	cmd::command_desc pause = play_command("play.pause", "Pause Play (the time stops, the worlds stay). Returns the play status");
	pause.enabled = [session] {
		const play_session* play = session();
		return play && play->state() == play_state::playing;
	};
	pause.invoke = [session, unavailable, reloading](const cmd::call& call) {
		play_session* play = session();
		if (!play) return unavailable();
		if (play->reloading()) return reloading();
		play->pause(reason_of(call));
		return cmd::result::success(status_of(play));
	};
	action_ui pause_ui = menu_item("Pause");
	// The command answers busy during a module reload; the menu item is greyed then.
	pause_ui.enabled = [session] {
		const play_session* play = session();
		return play && !play->reloading() && play->state() == play_state::playing;
	};
	out.push_back(actions.add(std::move(pause), std::move(pause_ui)));

	cmd::command_desc step = play_command("play.step",
		"While Play is paused, run n (1..600, default 1) frames of one fixed step each on the next frames, then pause again; "
		"while playing it fails: pause first. Returns the play status (frame counts the step's frames once they ran)");
	step.args_schema = json::parse(R"json({
		"type": "object",
		"properties": { "n": { "type": "integer", "minimum": 1, "maximum": 600, "description": "frames to run, default 1" } },
		"additionalProperties": false
	})json");
	step.enabled = [session] {
		const play_session* play = session();
		return play && play->active();
	};
	step.invoke = [session, unavailable, reloading](const cmd::call& call) {
		play_session* play = session();
		if (!play) return unavailable();
		if (play->reloading()) return reloading();
		if (play->state() == play_state::playing) return cmd::result::failure(cmd::codes::failed, "Play is running: pause first");
		std::int64_t frames = 1;
		if (const json::value* n = call.args.as_object().if_contains("n")) {
			const std::optional<std::int64_t> given = cmd::json_schema::integer_of(*n);
			if (!given || *given < 1 || *given > k_max_step_frames) {
				return cmd::result::failure(cmd::codes::invalid_args, "n is not an integer in 1..600");
			}
			frames = *given;
		}
		if (!play->step(static_cast<std::uint32_t>(frames), reason_of(call))) return unavailable();
		return cmd::result::success(status_of(play));
	};
	action_ui step_ui = menu_item("Step");
	step_ui.enabled = [session] {
		const play_session* play = session();
		return play && !play->reloading() && play->state() == play_state::paused;
	};
	out.push_back(actions.add(std::move(step), std::move(step_ui)));

	cmd::command_desc stop = play_command("play.stop",
		"Stop Play: the play worlds are destroyed and the editor is back as before Start. Returns the play status");
	stop.enabled = [session] {
		const play_session* play = session();
		return play && play->active();
	};
	stop.invoke = [session, unavailable](const cmd::call& call) {
		play_session* play = session();
		if (!play) return unavailable();
		play->stop(reason_of(call));
		return cmd::result::success(status_of(play));
	};
	action_ui stop_ui = menu_item("Stop");
	stop_ui.separator_before = true;
	out.push_back(actions.add(std::move(stop), std::move(stop_ui)));

	cmd::command_desc resume = play_command("play.resume", "Resume paused Play. Returns the play status");
	resume.enabled = [session] {
		const play_session* play = session();
		return play && !play->reloading() && play->state() == play_state::paused;
	};
	resume.invoke = [session, unavailable, reloading](const cmd::call& call) {
		play_session* play = session();
		if (!play) return unavailable();
		if (play->reloading()) return reloading();
		play->resume(reason_of(call));
		return cmd::result::success(status_of(play));
	};
	out.push_back(actions.add(std::move(resume), std::nullopt));

	cmd::command_desc status = play_command("play.status",
		"The play status: state (stopped, playing, paused), frame (frames the play set simulated since Start, Play and Step "
		"frames only; not app.advance's frame, which counts the editor's frames), worlds (of Play)");
	status.mutates = false;
	status.invoke = [session](const cmd::call&) { return cmd::result::success(status_of(session())); };
	out.push_back(actions.add(std::move(status), std::nullopt));
	return out;
}

edt::play_frame_commands edt::register_play_frame_commands(app::frame_command_registrations& core,
	cmd::command_registry& commands, app::frame_control& control, std::function<bool()> play_active, editor_events& events)
{
	// First: the same ids again would be refused.
	core = app::frame_command_registrations{};
	play_frame_commands out;
	out.commands = app::register_frame_commands(commands, control,
		app::frame_command_options{ .category = cmd::agent_category::play, .enabled = std::move(play_active) });
	out.release_on_stop = events.play_state_changed.subscribe([&control](play_state state, play_reason) {
		if (state == play_state::stopped) control.release();
	});
	return out;
}
