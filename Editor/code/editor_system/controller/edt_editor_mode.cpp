#include "edt_editor_mode.h"
#include "edt_play_session.h"
#include <snk/common/logger/engine_log.h>

std::optional<edt::editor_mode> edt::editor_mode_of(std::string_view name)
{
	if (name == "editor") return editor_mode::editor;
	if (name == "game") return editor_mode::game;
	return std::nullopt;
}

edt::editor_mode_controller::editor_mode_controller(play_session& play, editor_events& events)
	: m_play(play)
	, m_events(events)
{
	m_play_changed = events.play_state_changed.subscribe([this](play_state state, play_reason) {
		// A Step keeps Play paused and announces nothing new; a Stop or a resume ends the pause this controller made.
		if (!m_changing_play && state != play_state::paused) m_paused_play = false;
	});
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
		m_play.resume(reason);
		m_paused_play = false;
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
