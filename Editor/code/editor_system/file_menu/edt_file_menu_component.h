#pragma once
#include "edt_document_tabs.h"
#include "edt_document_tabs_bar.h"
#include "edt_save_all.h"
#include "edt_version_notice.h"
#include <snk/editor/edt_registration.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace res { class resource_system; }
namespace wnd { class window_system; }

namespace edt
{
	class asset_component;
	class asset_open_component;
	class editor_context;
	class file_menu;
	class import_component;
	class level_controller;
	class level_load_component;
	class prefab_editor_component;
	class problems_component;
	class scene_document;

	// The File and Edit menus of the editor and the commands behind them (file.*, edit.*): what each item asks of the level, the
	// other open documents, the import and the project (#1004). `file_menu` (controller/edt_file_menu.h) is the UI and the
	// action layer; this component hands it the hooks into the editor and registers the built-in menus, the file actions and the
	// Edit actions (undo, redo, delete...). It also knows how to start another editor on a project (File > Open Project).
	class file_menu_component
	{
	public:
		struct ports
		{
			editor_context& context;
			res::resource_system& resources;
			scene_document& state;
			level_controller& level_ctrl;
			level_load_component& level_load;
			prefab_editor_component& prefab;
			asset_open_component& asset_open;
			problems_component& problems;
			import_component& import;
			// The open project's manifest; nullopt without a project.
			std::optional<std::filesystem::path> project_manifest = std::nullopt;
			// Where New Project / Open Project start: next to the open project, else where the editor was started.
			std::filesystem::path projects_path;
			// Closes the active window (the end of the run).
			std::function<void()> exit = {};
			// Late-bound: null until the owner has made what answers them.
			std::function<void()> stop_play = {};
			std::function<std::optional<std::string>()> migration_blocker = {};
			std::function<std::optional<std::string>()> migration_stopped_reason = {};
			std::function<bool()> startup_pending = {};
			std::function<asset_component*()> assets = {};
			// The mesh modeller's questions (EPIC-77, #1011), late-bound: Exit, New and Open save or drop the meshes with unsaved edits, the
			// Unsaved list and the tabs bring a mesh's window to the front and close it through its own question.
			std::function<bool()> meshes_dirty = {};
			std::function<bool()> save_meshes = {};
			std::function<void()> discard_meshes = {};
			std::function<void(document_id)> show_mesh = {};
			std::function<void(document_id)> request_close_mesh = {};
		};

		explicit file_menu_component(ports from);
		~file_menu_component();
		file_menu_component(const file_menu_component&) = delete;
		file_menu_component& operator=(const file_menu_component&) = delete;

		// The built-in menus and the file.* actions.
		void install(std::vector<registration>& out);
		// The Edit actions (undo, redo, delete...): a separate step so that the owner can register what it does between the two
		// (the order of registrations is the order of the registry's list and of the tools an agent sees).
		void install_edit_actions(std::vector<registration>& out);
		// The window's close request goes through the "unsaved changes" flow and the exit blocker.
		void attach_window_close(wnd::window_system& windows);
		// The last save that upgraded a file, for a few seconds in the menu bar (#761); nothing otherwise.
		void draw_status();
		// "Unsaved (N)" in the menu bar while any document has unsaved changes; hovering lists them by kind, a click opens the
		// list as a popup (a row focuses its document, "Save All" runs it) (#1183). Draws nothing (no ImGui call) while all is
		// saved. One call in the menu bar status: move or drop it there.
		void draw_unsaved_indicator();
		// The strip of document tabs under the main menu (#1184): the scene, the prefab and the data documents, dirty ones marked.
		// A click activates, the close button goes through the document's own close question. One call in the dock host.
		void draw_document_tabs();

	private:
		// The rows of the unsaved list, one place (the popup of the menu bar item): a row focuses its document.
		void draw_unsaved_list(const std::vector<unsaved_group>& groups);
		void focus_unsaved(document_id id);
		// What bringing a data document's window or the prefab window to the front takes: shared by the Unsaved list and the tabs.
		document_focus_ports focus_ports();
		std::optional<std::string> migration_blocker() const;

		ports m_ports;
		std::unique_ptr<file_menu> m_menu;
		timed_notice m_save_notice;
		std::unique_ptr<document_tabs> m_tabs;
		document_tabs_bar m_tabs_bar;
	};
}
