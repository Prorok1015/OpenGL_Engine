#pragma once
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include "edt_agent_model_import.h"
#include <snk/project/prj_create.h>
#include <snk/window/wnd_window_system.h>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// What a level file below the resource root is: an agent may open or overwrite only a plain file.
	enum class level_file_state : std::uint8_t
	{
		missing,
		file,
		unsafe,	// a link, a folder or anything else on the way or at the end
	};

	// Looks at `relative_path` (already checked by agent_level_path) below `root` without following links.
	level_file_state inspect_level_file(const std::filesystem::path& root, const std::string& relative_path);

	// The level path an agent may name: `res://levels/a.desc` or `levels/a.desc`. A path with another protocol, a drive
	// letter, a leading '/', a backslash, a control character, an empty, '.' or '..' segment, or not ending in `.desc`
	// is refused (`error` says why); so is a name Windows treats as a device (CON, NUL, COM1, ... with any extension), a
	// segment ending in '.' or ' ' and the characters <>\"|?*; otherwise `path` is the project-relative path without the protocol.
	struct agent_level_path
	{
		std::string path;
		std::string error;
		bool ok() const { return error.empty(); }
	};
	agent_level_path check_agent_level_path(std::string_view text);
	// A folder below res://: the same rules, no extension.
	agent_level_path check_agent_folder(std::string_view text);
	// One name (an asset's): the same rules, a single segment.
	agent_level_path check_agent_name(std::string_view text);

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
		// The documents besides the scene (data documents): unsaved changes among them count for New, Open and Exit, the
		// save of the prompt writes them first, discard drops them. Left empty, there are none.
		std::function<bool()> other_documents_dirty;
		std::function<bool()> save_other_documents;
		std::function<void()> discard_other_documents;
		std::function<void(const std::filesystem::path&)> load_level; // absolute path picked by the user
		// Starts loading the level at a project-relative path (`levels/a.desc`) without a round trip through a file
		// system path: what the commands an agent calls use. Null: those commands answer `failed`.
		// Returns the reason when the file cannot be loaded (it is not a level).
		std::function<std::optional<std::string>(const std::string&)> load_level_tag;
		// What the level file at a project-relative path is, for the commands an agent calls: looked at in every layer of
		// the resources (a lower content root, the engine), not only the top one. Null: looked at below `resources_path`
		// with inspect_level_file.
		std::function<level_file_state(const std::string&)> inspect_level;
		// Why the level may not be written now (a level is loading: the document does not belong to its file yet):
		// file.save and file.save_as answer `busy` with it, whoever calls. Null or nullopt: they run.
		std::function<std::optional<std::string>()> level_write_blocker;
		// The import of a model an agent asks for, without a dialog (#548). Null: model import is not built in.
		agent_model_import* agent_import = nullptr;
		// Why the level may not be replaced now (the start level of --game is loading, #518): file.new and file.open
		// answer `busy` with it, whoever calls. Null or nullopt: they run.
		std::function<std::optional<std::string>()> level_change_blocker;
		std::function<void(const std::filesystem::path&)> import_model;
		std::function<void()> exit;
		std::filesystem::path resources_path;  // where "Open Level" starts
		// Starts another editor on the project (its manifest file); this one exits afterwards. The error, if it did not start.
		std::function<std::optional<std::string>(const std::filesystem::path& manifest_file, bool allow_engine_version_mismatch)> open_project;
		// Makes a project in `folder` from the editor's template.
		std::function<prj::create_result(const std::filesystem::path& folder, const std::string& name)> create_project;
		std::filesystem::path projects_path;   // where "New Project" and "Open Project" start
		// File > Recent Projects, most recent first (manifest files); the one open now is left out by the caller.
		std::vector<std::filesystem::path> recent_projects;
		// Why the editor may not exit or switch projects now (a project migration writes files): file.exit,
		// file.open_project and the recent projects answer `busy` with it, and exit() (File > Exit, the window's close
		// request) shows it instead of asking. Null or nullopt: they run.
		std::function<std::optional<std::string>()> exit_blocker;
	};

	// The File menu: New / Open / Save / Save As / Import / New Project / Open Project / Exit, with the "unsaved
	// changes" flow for everything that leaves the level. Each item is two layers. The action (file.new, file.open,
	// file.save, file.save_as, file.import_model, file.new_project, file.open_project, file.exit) takes everything as
	// JSON arguments and works without any UI. The methods below are the
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
		bool anything_dirty() const;
		void discard_everything();
		void import_model();
		// Asks for a folder, makes a project there from the template and switches to it (EPIC-42 #158).
		void new_project();
		// Asks for a project.snkproj and switches to it. Switching starts another editor and exits this one.
		void open_project();
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
		// invoke() for what a person started from the menu: a failure is also shown to them, after `failure`.
		bool invoke_or_report(std::string_view action_id, boost::json::value args, std::string_view failure);
		// Checks that `project` (a manifest or its folder) opens, starts another editor on it and exits this one. The
		// caller already settled the unsaved changes.
		action_result switch_to_project(const std::filesystem::path& project, bool allow_engine_version_mismatch);
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
