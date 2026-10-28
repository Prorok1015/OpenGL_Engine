#pragma once
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/window/wnd_window_system.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	// What the File menu needs from the rest of the editor. Plain callbacks, so the menu flow can run
	// without a level, a resource system or ImGui.
	struct file_menu_hooks
	{
		std::function<bool()> has_scene;      // a level is loaded (there is something to save)
		std::function<bool()> is_dirty;
		std::function<std::string()> level_path; // res-relative path of the saved level, empty if untitled
		std::function<void()> new_level;
		std::function<bool()> quick_save;     // overwrite the saved level, true if written
		std::function<bool(const std::string&)> save_level_as; // true if written
		std::function<void()> discard_changes;
		std::function<void(const std::filesystem::path&)> load_level; // absolute path picked by the user
		std::function<void(const std::filesystem::path&)> import_model;
		std::function<void()> exit;
		std::filesystem::path resources_path;  // where "Open Level" starts
	};

	// The File menu: New / Open / Save / Save As / Import / Exit, with the "unsaved changes" flow for New and Exit.
	// Each item is two layers. The action (file.new, file.open, file.save, file.save_as, file.import_model,
	// file.exit) takes everything as JSON arguments and works without any UI. The methods below are the
	// frontend a click or a shortcut runs: they ask through dialog_service and then invoke the action.
	class file_menu
	{
	public:
		file_menu(dialog_service& dialogs, action_registry& actions, file_menu_hooks hooks);

		void new_level();
		void open_level();
		// Save As: always asks for a path (Ctrl+Shift+S).
		void save_level();
		// Save: overwrites the saved level, asks for a path if it has none yet (Ctrl+S).
		void quick_save();
		void import_model();
		void exit();

		// Sends the window's own close request (X button, Alt+F4) through exit(), so it asks about unsaved
		// changes like File > Exit does and Cancel keeps the editor open.
		void attach_window_close(wnd::window_system& windows);

		// Puts the File menu items (and their shortcuts) into the registry. They stay registered while this
		// menu lives; call it once.
		void register_actions();

	private:
		// Runs `action` right away (`discard` false), or after the user settles the unsaved changes (`discard`
		// true: whatever is still unsaved may go). Returns the prompt it queued, if any.
		std::optional<dialog_service::dialog_id> guard_unsaved(std::function<void(bool discard)> action);
		void on_unsaved_choice(unsaved_choice choice, const std::function<void(bool discard)>& action);
		// False (and logged) if the action refused.
		bool invoke(std::string_view action_id, boost::json::value args);
		// invoke() for file.save / file.save_as from the menu: a failed write is also shown to the user, not only logged.
		bool save_or_report(std::string_view action_id, boost::json::value args);
		// One Save As dialog at a time; `then` (if any) runs after it wrote the level.
		void ask_save_path(std::function<void()> then);

		dialog_service& m_dialogs;
		action_registry& m_actions;
		file_menu_hooks m_hooks;
		std::optional<dialog_service::dialog_id> m_save_as_id;
		std::optional<dialog_service::dialog_id> m_exit_prompt_id;
		std::vector<std::function<void()>> m_after_save;
		// The dialog queue belongs to editor_context and can outlive this menu: a dialog answered after
		// that must not call back into it (or into the hooks, which capture editor_system).
		std::shared_ptr<void> m_alive = std::make_shared<int>(0);
		// Last: the actions capture `this` and are removed before the rest is destroyed.
		std::vector<registration> m_registrations;
	};
}
