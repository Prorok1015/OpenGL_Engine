#pragma once
#include <snk/editor/edt_dialog_service.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	class dockspace;

	// What the File menu needs from the rest of the editor. Plain callbacks, so the menu flow can run
	// without a level, a resource system or ImGui.
	struct file_menu_hooks
	{
		std::function<bool()> has_scene;      // a level is loaded (there is something to save)
		std::function<bool()> is_dirty;
		std::function<std::string()> level_path; // res-relative path of the saved level, empty if untitled
		std::function<void()> new_level;
		std::function<void()> quick_save;     // overwrite the saved level
		std::function<bool(const std::string&)> save_level_as; // true if written
		std::function<void()> discard_changes;
		std::function<void(const std::filesystem::path&)> load_level; // absolute path picked by the user
		std::function<void(const std::filesystem::path&)> import_model;
		std::function<void()> exit;
		std::filesystem::path resources_path;  // where "Open Level" starts
	};

	// The File menu: New / Open / Save / Import / Exit, with the "unsaved changes" flow for New and Exit.
	class file_menu
	{
	public:
		file_menu(dialog_service& dialogs, file_menu_hooks hooks);

		void new_level();
		void open_level();
		void save_level();
		void quick_save();
		void import_model();
		void exit();

		void attach(dockspace& menu);

	private:
		// Runs `action` right away, or after the user settles the unsaved changes.
		void guard_unsaved(std::function<void()> action);
		void on_unsaved_choice(unsaved_choice choice, const std::function<void()>& action);
		// One Save As dialog at a time; `then` (if any) runs after it wrote the level.
		void ask_save_path(std::function<void()> then);

		dialog_service& m_dialogs;
		file_menu_hooks m_hooks;
		std::optional<dialog_service::dialog_id> m_save_as_id;
		std::vector<std::function<void()>> m_after_save;
		// The dialog queue belongs to editor_context and can outlive this menu: a dialog answered after
		// that must not call back into it (or into the hooks, which capture editor_system).
		std::shared_ptr<void> m_alive = std::make_shared<int>(0);
	};
}
