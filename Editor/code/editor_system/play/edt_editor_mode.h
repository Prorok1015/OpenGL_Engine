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
	// session that ended or a pause that is no longer its own. With a UI, any Stop in game mode (a person's, an agent's,
	// a level load, a module reload) brings the editor layout back. Raises editor_events::editor_mode_changed. No ImGui,
	// no GL. Main thread.
	class editor_mode_controller
	{
	public:
		// `has_ui` false (headless): there is no layout to go back to, a Stop leaves the mode as it is.
		editor_mode_controller(play_session& play, editor_events& events, bool has_ui = true);
		editor_mode_controller(const editor_mode_controller&) = delete;
		editor_mode_controller& operator=(const editor_mode_controller&) = delete;

		editor_mode mode() const { return m_mode; }
		// Nullopt: the editor is in `mode` now (also when it already was). Otherwise why not (Play did not start, see
		// play_session::start_refusal), and the mode and Play are as they were. `reason` goes to Play's changes.
		std::optional<std::string> set_mode(editor_mode mode, play_reason reason = play_reason::user);
		std::optional<std::string> toggle(play_reason reason = play_reason::user);

		// The play.* command whose change set_mode(`mode`) would make now (play.start, play.resume or play.pause); empty
		// when it would not change Play. editor.mode.toggle asks the agent policy about it, so the toggle opens nothing
		// the play.* commands themselves keep closed.
		std::string_view play_command_for(editor_mode mode) const;

		// Right after play_session::after_module_swap (the swap gate's hook): Play that a reload stopped in game mode and
		// that did not start again leaves the game layout (WARN).
		void after_module_swap();

		// Play is paused by this controller's switch to the editor, and nothing else changed Play since.
		bool holds_pause() const { return m_paused_play; }

		// The start level of --game (#518): from begin_startup until finish_startup the editor shows a loading screen
		// with `level_label` (the start level's res:// tag) instead of its layout. Only when that load is scheduled.
		void begin_startup(std::string level_label);
		bool startup_pending() const { return m_startup_pending; }
		const std::string& startup_level_label() const { return m_startup_label; }
		// Every end of that load comes here, and it always ends the loading screen. `loaded` false (the load failed or
		// was cancelled): ERROR, editor mode. `exiting` (editor/exit_after_level_loaded stops the loop): editor mode, Play
		// would only be torn down. Otherwise set_mode(game); a refused start keeps the editor mode.
		void finish_startup(bool loaded, bool exiting);

	private:
		play_session& m_play;
		editor_events& m_events;
		bool m_has_ui = true;
		editor_mode m_mode = editor_mode::editor;
		bool m_paused_play = false;
		bool m_startup_pending = false;
		std::string m_startup_label;
		// The last change of Play was a reload's Stop while in game mode.
		bool m_reload_stopped_game = false;
		// Play's changes the controller makes itself are not someone else's.
		bool m_changing_play = false;
		connection m_play_changed;
	};
}
