#include "edt_editor_mode.h"
#include "edt_play_session.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_path_sanitize.h>
#include <format>
#include <utility>

std::optional<edt::editor_mode> edt::editor_mode_of(std::string_view name)
{
	if (name == "editor") return editor_mode::editor;
	if (name == "game") return editor_mode::game;
	return std::nullopt;
}

bool edt::is_game_mode_shortcut(std::string_view action_id)
{
	return action_id == "editor.mode.toggle" || action_id == "play.start" || action_id == "play.pause"
		|| action_id == "play.step" || action_id == "play.stop";
}

std::string edt::game_level_problem(std::string_view value)
{
	constexpr std::string_view scheme = "res://";
	if (!value.starts_with(scheme)) return "only a res:// tag is taken (e.g. res://levels/main.desc)";
	const std::string_view path = value.substr(scheme.size());
	if (path.find('\\') != std::string_view::npos) return "the path holds '\\'";
	if (std::string problem = res::portable_path_problem(path); !problem.empty()) return std::format("the path is not portable: {}", problem);
	return {};
}

edt::editor_mode_controller::editor_mode_controller(play_session& play, editor_events& events, bool has_ui)
	: m_play(play)
	, m_events(events)
	, m_has_ui(has_ui)
{
	m_play_changed = events.play_state_changed.subscribe([this](play_state state, play_reason reason) {
		if (m_changing_play) return;
		// A Step keeps Play paused and announces nothing new; a Stop or a resume ends the pause this controller made.
		if (state != play_state::paused) m_paused_play = false;
		// Owner decision: nothing to play in the game layout once Play stops; a module reload starts it again at once.
		const bool reload = reason == play_reason::stopped_for_reload || reason == play_reason::module_reloaded;
		m_reload_stopped_game = state == play_state::stopped && reason == play_reason::stopped_for_reload && m_mode == editor_mode::game;
		if (state == play_state::stopped && !reload && m_has_ui && m_mode == editor_mode::game) {
			m_mode = editor_mode::editor;
			egLOG_INFO("editor", "editor mode: Play stopped");
			m_events.editor_mode_changed.emit(m_mode);
		}
	});
}

void edt::editor_mode_controller::after_module_swap()
{
	if (!std::exchange(m_reload_stopped_game, false) || m_play.state() != play_state::stopped) return;
	// play_session::after_module_swap did not start Play again (a start blocker, the level gone): nothing to play.
	egLOG_WARN("editor", "game mode: Play did not start again after the module reload");
	if (!m_has_ui || m_mode != editor_mode::game) return;
	m_mode = editor_mode::editor;
	m_events.editor_mode_changed.emit(m_mode);
}

void edt::editor_mode_controller::begin_startup(std::string level_label)
{
	m_startup_label = std::move(level_label);
	m_startup_pending = true;
}

void edt::editor_mode_controller::finish_startup(bool loaded, bool exiting)
{
	const std::string level = std::exchange(m_startup_label, std::string{});
	m_startup_pending = false;
	if (!loaded) {
		egLOG_ERROR("editor", "game mode not entered: the start level '{}' did not load (failed or cancelled)", level);
		return;
	}
	if (exiting) {
		egLOG_INFO("editor", "game mode not entered: the editor exits after the level loaded");
		return;
	}
	// A refusal is logged by set_mode; the editor stays in editor mode.
	(void)set_mode(editor_mode::game);
}

std::string_view edt::editor_mode_controller::play_command_for(editor_mode mode) const
{
	if (mode == m_mode) return {};
	if (mode == editor_mode::editor) return m_play.state() == play_state::playing ? "play.pause" : "";
	if (m_play.state() == play_state::stopped) return "play.start";
	if (m_play.state() == play_state::paused && m_paused_play) return "play.resume";
	return {};
}

std::optional<std::string> edt::editor_mode_controller::set_mode(editor_mode mode, play_reason reason)
{
	if (mode == m_mode) return std::nullopt;

	m_changing_play = true;
	if (mode == editor_mode::editor) {
		if (m_play.state() == play_state::playing && m_play.pause(reason)) m_paused_play = true;
	} else if (m_play.state() == play_state::stopped) {
		if (auto refused = m_play.start_refusal()) {
			m_changing_play = false;
			egLOG_WARN("editor", "game mode not entered: Play cannot start: {}", *refused);
			return refused;
		}
		if (!m_play.start(reason)) {
			m_changing_play = false;
			return std::string{ "Play did not start: see the log" };
		}
	} else if (m_play.state() == play_state::paused && m_paused_play) {
		if (m_play.resume(reason)) {
			m_paused_play = false;
		} else {
			// Still paused and still this controller's pause: the next switch to the game tries again.
			egLOG_WARN("editor", "game mode entered, but Play did not resume");
		}
	}
	m_changing_play = false;

	m_mode = mode;
	egLOG_INFO("editor", "{} mode", editor_mode_name(mode));
	m_events.editor_mode_changed.emit(mode);
	return std::nullopt;
}

std::optional<std::string> edt::editor_mode_controller::toggle(play_reason reason)
{
	return set_mode(m_mode == editor_mode::editor ? editor_mode::game : editor_mode::editor, reason);
}
