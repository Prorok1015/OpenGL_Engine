#pragma once
#include <snk/editor/edt_editor_events.h>
#include <optional>
#include <string>
#include <string_view>

namespace edt
{
	class play_session;

	// "editor" / "game" (editor/start_mode, the mode argument of editor.mode.toggle); nullopt for anything else.
	std::optional<editor_mode> editor_mode_of(std::string_view name);

	// The actions whose shortcuts fire in game mode: editor.mode.toggle and the Play menu. Every other key belongs to
	// the game there.
	bool is_game_mode_shortcut(std::string_view action_id);

	// Why `value` of --game=<level> (editor/game_level) cannot be the start level; empty when it can. Only a res:// tag
	// with a portable relative path (res::portable_path_problem) is taken, never a file system path or another scheme.
	std::string game_level_problem(std::string_view value);

	// Which layout the editor shows (#294, AI_INTEGRATION.md §3.8) and what that does to Play: game mode plays, editor
	// mode edits. Going to the editor pauses a running Play and remembers that it did; going back to the game resumes
	// only that pause (a pause someone else made stays), and starts Play when it is stopped. The note is dropped by any
	// change of Play the controller did not make other than a Step (a Stop, a resume): a later switch never resumes a
	// session that ended or a pause that is no longer its own. Raises editor_events::editor_mode_changed. No ImGui, no
	// GL. Main thread.
	class editor_mode_controller
	{
	public:
		editor_mode_controller(play_session& play, editor_events& events);
		editor_mode_controller(const editor_mode_controller&) = delete;
		editor_mode_controller& operator=(const editor_mode_controller&) = delete;

		editor_mode mode() const { return m_mode; }
		// Nullopt: the editor is in `mode` now (also when it already was). Otherwise why not (Play did not start, see
		// play_session::start_refusal), and the mode and Play are as they were. `reason` goes to Play's changes.
		std::optional<std::string> set_mode(editor_mode mode, play_reason reason = play_reason::user);
		std::optional<std::string> toggle(play_reason reason = play_reason::user);

		// Play is paused by this controller's switch to the editor, and nothing else changed Play since.
		bool holds_pause() const { return m_paused_play; }

	private:
		play_session& m_play;
		editor_events& m_events;
		editor_mode m_mode = editor_mode::editor;
		bool m_paused_play = false;
		// Play's changes the controller makes itself are not someone else's.
		bool m_changing_play = false;
		connection m_play_changed;
	};
}
