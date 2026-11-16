#pragma once
#include "edt_world_problems.h"
#include "edt_desc_problems.h"
#include "edt_play_preflight.h"
#include "edt_desc_file_commands.h"
#include "edt_version_notice.h"
#include "edt_project_trust_service.h"
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_types.h>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <snk/resource/resources/res_resource_handle.hpp>
#include "edt_editor_camera.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_editor_services.h>
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "edt_level_controller.h"
#include "edt_migration_startup.h"
#include "edt_module_swap_gate.h"
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/scene/level/scn_start_level.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <vector>

// Forward declarations — full types only needed in .cpp
namespace desc { class desc_system; class desc_base; }
namespace res  { class resource_system; }
namespace rnd  { class render_system; }
namespace gui  { class gui_system; }
namespace scn  { class ecs_assembler; class level_desc; }
namespace ecs  { class system_factory; }
namespace inp  { class input_router; }
namespace cmd  { class command_registry; }

namespace edt {
	class asset_component;
	class panels_component;
	class problems_component;
	class level_load_component;
	class asset_database;
	class agent_server;
	class agent_access;
	class agent_component;
	class agent_ui_component;
	class theme_settings_component;
	class file_menu;
	class game_build_component;
	class project_trust_service;
	class vcs_component;
	class migration_component;
	struct journal_offer;
	class live_mode_component;
	class play_component;
	class editor_context;
	class editor_layer;
	class asset_open_component;
	class import_component;
	class scene_hierarchy_panel;
	class inspector_panel;
	class viewport_panel;
	class game_view_panel;
	class console_panel;
	class problems_panel;
	class blackboard_panel;
	class input_actions_panel;
	class build_settings_panel;
	class git_panel;
	class world_systems_panel;
	class asset_browser_panel;
	class prefab_editor_component;
	class play_session;
	class editor_mode_controller;

	class editor_system
	{
	public:
		editor_system(desc::desc_system& desc_system, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys);
		// Closes the prefab preview and releases the scene document in the editor_context before its members die.
		~editor_system();

		void init(ds::app_data_storage& inp_sys);

		// Created in init() and owned by the app_data_storage; null before init().
		editor_context* context() const { return m_context; }
		play_session* play() const { return m_play; }

		// False (and nothing changes) when the file is there but is not a level: loading, say, a material as a level
		// would assert in the desc adapter.
		bool begin_level_load(const res::tag& level_tag);
		// True from begin_level_load until the level is loaded, failed or cancelled. The document still belongs to the
		// level it had before; only a successful load makes it the new one.
		bool level_load_in_progress() const;
		// The level file the document belongs to (empty for an untitled one); not the one being loaded.
		const res::tag& current_level_tag() const { return m_state.level_tag; }
		// The working copy the document's level is read from (`memory://editor/loading/<n>/<name>.desc`).
		const res::tag& current_editor_tag() const { return m_state.editor_tag; }
		// The level file hot reload follows (empty while a load runs).
		const res::tag& watched_level_tag() const { return m_level_ctrl.watched_tag(); }

		// The headless `ui` stage: the per-frame callbacks (level/asset loads, dialogs, progress windows, next-frame
		// work) without the dockspace and the panels. They still use ImGui: call inside gui_system::run_without_drawing.
		void run_implicit_callbacks();
		// Live mode's frame (#445): the `editor_live` frame stage, after the job pump.
		void tick_live_mode();
		// Once a frame: a loaded level waits for the loads it started before "level loaded" and the exit (#669).
		void tick_level_settle();
		// What a row of Problems (double click, Enter, "Go to") leads to: its node selected in its world, or its file
		// opened when an asset kind opens it; nothing for a node that is gone or a level that is not open (#596).
		void go_to_problem(const std::string& source, const problem& item);
		// The repository watcher's frame (#485): the `editor_vcs` frame stage, see vcs_component::tick.
		void tick_repository_watcher();
		// The source "assets" of Problems follows the asset database: the `editor_assets` frame stage.
		void tick_asset_problems();
		// A person is in the middle of an edit: an active ImGui item other than a text field that only has the
		// keyboard, a value the inspector has not committed yet, a gizmo ring being dragged. An agent's edit waits.
		bool person_is_editing() const;

	private:
		editor_context* m_context = nullptr;
		// The context is registered after this system, so a storage that is torn down without editor_term() destroys
		// it first: the destructor reaches it through this, never through m_context.
		std::weak_ptr<editor_context> m_context_alive;

		// The engine services the controllers, the documents and the editor_context work through; filled in init().
		// Declared before everything that refers to it, so it is destroyed after all of it. Document reloads never touch it.
		editor_services m_services;

		// The level being edited; the controllers share it. The documents of the editor_context refer to it.
		scene_document m_state;

		// Controllers
		scene_editor m_scene_editor;
		world_controller m_world_ctrl;
		level_controller m_level_ctrl;

		// Services (not owned): the base ones, for the importer, the exporter and the editor's assets. The game data
		// goes through m_services.
		desc::desc_system&    desc_system;
		res::resource_system& m_res;
		rnd::render_system&   m_rnd;
		gui::gui_system&      m_gui;

		// Input
		inp::input_router* m_input_router = nullptr;

		// Import/export (the decoder, the person's import, the exporter, the agent's import)
		std::unique_ptr<edt::import_component> m_import;

		// init() steps: the panels, their dependencies and the callbacks between panels and controllers.
		void wire_controller_callbacks();
		// Play (#39): the session, the locks of the panels, the Play menu and the status in the menu bar.
		void create_play_session(ds::app_data_storage& data);
		// The last save that upgraded a file, for a few seconds (#761); nothing otherwise.
		void draw_save_notice();
		// Stops a running live build through game_builder::stop_background (never the person's build, and a request of
		// the person's waiting for it stays queued); the outcome reaches live mode, which builds the changes again.
		void stop_live_build();

	public:
		agent_server* agent_server_object();
		// The policy behind the agent server; null before init(). For tests that pin what an agent can never reach.
		agent_access* agent_access_object();
		// The asset database of the open project (#760); null without a project.
		asset_database* assets();

	private:
		// Editor layer & panels
		std::shared_ptr<edt::editor_layer> editor_layer;

		// What the Problems panel shows and where a row leads (#1004): the publishers sit here. Before the panels, the prefab
		// editor, the loads and the drop, which report into them, so it goes after them.
		std::unique_ptr<problems_component> m_problems;

		// The editor's panels (#1004): made, wired to the controllers and registered by panels_component, in the View menu order.
		std::unique_ptr<panels_component> m_panels;

		// The prefab editor (EPIC-25, #1004): its window, its Inspector hand-over, its close question. After the panels it
		// reaches, so it goes before them; the preview document itself is owned by the editor_context's document_manager.
		std::unique_ptr<prefab_editor_component> m_prefab;

		// Opening an asset and adding one to the scene, with the data documents (EPIC-56, #793, #1004).
		std::unique_ptr<asset_open_component> m_asset_open;

		// A game module loaded or unloaded: the editor lets go of the old game layer, then rebuilds its level in the
		// new one (level_controller::detach_from_game_layer / attach_to_game_layer).
		void follow_game_layer_change(bool before);
		// Changes of the game layer reach follow_game_layer_change through it; a reload of the game module holds them (#435).
		module_swap_gate m_swap_gate{ [this](bool before) { follow_game_layer_change(before); } };
		// The level whose load the change interrupted; loaded again into the new layer.
		res::tag m_level_after_layer_change;
		// Moves with every change of the game layer: project.migrate binds the plan a person approved to the code that runs it.
		std::uint64_t m_game_layer_generation = 0;
		std::optional<std::string> migration_blocker() const;
		// What refuses a write into the project: a migration that runs, else one that stopped half way and is not rolled back.
		std::optional<std::string> project_write_blocker();
		timed_notice m_save_notice;
		// A level or a dropped asset is loading: Play does not start and a module reload waits (#600).
		std::optional<std::string> load_in_progress() const;

		std::function<void()> m_exit_action;
		// Set with editor/exit_after_level_loaded: stops the frame loop once a loaded level settled (CI headless run).
		std::function<void()> m_stop_after_level_loaded;
		ds::task_system* m_tasks = nullptr;
		// The load of a level and the settle after it (US-24-4, #613, #1004); its action is in m_registrations, removed before it.
		// Declared after m_tasks, which the settle uses.
		std::unique_ptr<edt::level_load_component> m_level_load;
		// Project migration (EPIC-58, #761, #1004): the commands, the window, the offer to roll back, the notice and the scan;
		// declared after what its scan callback touches, so the scan is cancelled first. A running migration refuses Play, level
		// changes and saves, builds, module reloads and quitting (migration_blocker).
		std::unique_ptr<migration_component> m_migration;
		std::unique_ptr<edt::file_menu> m_file_menu;
		// Build > Build Game and the reload of the game module (#434, #435, #1004); after the file menu, so it is destroyed first
		// (its job is stopped in the destructor), and after the swap gate it hands to the host.
		std::unique_ptr<edt::game_build_component> m_build;
		// Version control and the code host (#458, #1004): git, the repository watcher, pull requests, the tokens and the Git
		// Account window; its commands are in m_registrations, removed before it. Before the asset database, whose callbacks ask it
		// whether a git write runs, so the database goes first.
		std::unique_ptr<edt::vcs_component> m_vcs;
		// The asset database and what hangs on it (#760, #770, #1004): follows res:// from init, writes its cache when it goes;
		// null without a project. Its commands are in m_registrations, removed before it.
		std::unique_ptr<edt::asset_component> m_asset_component;
		// The person's trust in the project (#921); owned by the app_data_storage, where main's trust_library finds
		// it.
		std::weak_ptr<edt::project_trust_service> m_trust;
		// Live mode and the question "Trust this project?" (#445, #921, #1004); after the builder and the reloader it calls, so it
		// is destroyed first.
		std::unique_ptr<edt::live_mode_component> m_live;
		// Play of the scene document and the editor/game mode (#1004); its commands are in m_registrations, removed before it.
		// Stopped in the destructor while the context it reports to is still there.
		std::unique_ptr<edt::play_component> m_play_component;
		// The component's session and mode controller, for the areas that ask whether Play runs; null before the component.
		// Valid only while m_play_component lives: no member destroyed after it may use them.
		edt::play_session* m_play = nullptr;
		edt::editor_mode_controller* m_modes = nullptr;

		// Selection routing between the panels; last member, so it is dropped before the panels are.
		std::vector<connection> m_selection_connections;
		// Panels and menus in the editor_context registries; removed first, before the panels they point at.
		std::vector<registration> m_registrations;

		// What an agent can reach (the MCP server, the policy, the log filter, editor.quit). Declared before the agents window
		// (m_agent_ui) below, which reads its access: that goes first, then this one (shutdown() already ran at the top of ~editor_system).
		std::unique_ptr<agent_component> m_agent;
		// The agents window, its status slot and notice (#449): reads the access of m_agent, so it is destroyed first.
		std::unique_ptr<agent_ui_component> m_agent_ui;
		// The editor's theme and the Editor Settings window (#1109).
		std::unique_ptr<theme_settings_component> m_theme_settings;
	};
}
