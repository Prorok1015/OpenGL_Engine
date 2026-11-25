#pragma once
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_registration.h>
#include <filesystem>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gs { class input_actions; }
namespace gui { class gui_system; }
namespace inp { class input_router; }
namespace res { class resource_system; }
namespace rnd { class render_system; class capture_service; }

namespace edt
{
	class asset_browser_panel;
	class thumbnail_disk_cache;
	class thumbnail_service;
	class blackboard_panel;
	class build_settings_panel;
	class console_panel;
	class editor_context;
	class game_view_panel;
	class git_panel;
	class input_actions_panel;
	class inspector_panel;
	class level_controller;
	class panel_base;
	class ui_probe;
	class problems_component;
	class problems_panel;
	class scene_document;
	class scene_editor;
	class scene_hierarchy_panel;
	class viewport_panel;
	class world_controller;
	class world_systems_panel;
	enum class dock_slot;
	struct editor_services;
	namespace picker { class catalog; }

	// The editor's panels, whole: they are made here, given what they need, and registered in the editor_context in one order
	// (registration order is the render order and the View menu order). The panels of the migration and the agents are made
	// elsewhere and join through add(). editor_system creates it first (panels made and wired to the controllers), calls
	// install() where the windows go in, and reaches a panel through the accessors.
	class panels_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_services& services;
			scene_document& state;
			scene_editor& scene;
			world_controller& worlds;
			res::resource_system& resources;
			rnd::render_system& rnd;
			gui::gui_system& gui;
			inp::input_router* input_router;
			problems_component& problems;
			// Null in a headless run without the engine's capture service.
			rnd::capture_service* capture;
			// The console shows the engine log; off where the log text differs from run to run (screenshots).
			bool capture_log;
			// The pickers' catalog; null without a project: read when asked.
			std::function<const picker::catalog*()> asset_catalog;
			// Null without a project.
			std::function<std::optional<std::filesystem::path>()> project_root;
			// The project's cache folder (prj::project_context::cache_dir()) where the thumbnails are kept between runs, and
			// whether the project is trusted (project_trust_service::trusted): on an untrusted or no project they stay in memory.
			std::function<std::optional<std::filesystem::path>()> project_cache_dir;
			std::function<bool()> project_trusted;
			std::function<gs::input_actions*()> input_actions;
			// "debuggame" or "release": the Build Settings window's default.
			std::string default_configuration;
			std::function<bool()> manual_build_running;
			// A live build is stopped instead of waiting: its configure would clear the --fresh mark a define removal writes.
			std::function<void()> stop_live_build;
			std::function<bool()> play_active;
		};

		explicit panels_component(ports from);
		~panels_component();
		panels_component(const panels_component&) = delete;
		panels_component& operator=(const panels_component&) = delete;

		// Registers every panel in the View menu order. `after_assets` runs after the Asset Browser: where the prefab editor's window
		// goes in.
		void install(std::vector<registration>& out, const std::function<void()>& after_assets);
		// A panel made elsewhere (Project Migration, MCP / Agents) joins the registry.
		void add(std::vector<registration>& out, const std::shared_ptr<panel_base>& panel, dock_slot slot, bool open_by_default);

		// The hierarchy, inspector, viewport, console and asset browser exist from the constructor on; the game view, problems,
		// blackboard, input actions, build settings, git and world systems panels are made by install() and are null before it.
		std::shared_ptr<scene_hierarchy_panel>& hierarchy() { return m_hierarchy; }
		std::shared_ptr<inspector_panel>& inspector() { return m_inspector; }
		std::shared_ptr<viewport_panel>& viewport() { return m_viewport; }
		std::shared_ptr<game_view_panel>& game_view() { return m_game_view; }
		std::shared_ptr<console_panel>& console() { return m_console; }
		std::shared_ptr<problems_panel>& problems() { return m_problems_panel; }
		std::shared_ptr<blackboard_panel>& blackboard() { return m_blackboard; }
		std::shared_ptr<input_actions_panel>& input_actions() { return m_input_actions; }
		std::shared_ptr<build_settings_panel>& build_settings() { return m_build_settings; }
		std::shared_ptr<git_panel>& git() { return m_git; }
		std::shared_ptr<world_systems_panel>& world_systems() { return m_world_systems; }
		std::shared_ptr<asset_browser_panel>& asset_browser() { return m_asset_browser; }
		// The one probe of the headless smoke (editor/ui_probe), shared by the panels and the main menu; null without it.
		std::shared_ptr<ui_probe> probe() const { return m_ui_probe; }

	private:
		void wire_scene_callbacks();
		std::unique_ptr<thumbnail_service> make_thumbnail_service();
		// The cache the thumbnails use now (null: none), made, kept or closed after the project and its trust as they are.
		std::shared_ptr<thumbnail_disk_cache> refresh_thumbnail_cache();

		ports m_ports;
		// The console's feed from the engine log; removed with the panels.
		engine::log_sink_handle m_log_sink;
		std::shared_ptr<ui_probe> m_ui_probe;
		std::shared_ptr<scene_hierarchy_panel> m_hierarchy;
		std::shared_ptr<inspector_panel> m_inspector;
		std::shared_ptr<viewport_panel> m_viewport;
		std::shared_ptr<game_view_panel> m_game_view;
		std::shared_ptr<console_panel> m_console;
		std::shared_ptr<problems_panel> m_problems_panel;
		std::shared_ptr<blackboard_panel> m_blackboard;
		std::shared_ptr<input_actions_panel> m_input_actions;
		std::shared_ptr<build_settings_panel> m_build_settings;
		std::shared_ptr<git_panel> m_git;
		std::shared_ptr<world_systems_panel> m_world_systems;
		std::shared_ptr<asset_browser_panel> m_asset_browser;
		// The asset browser's thumbnails (#1192); the lifetime is declared after it and after the cache state so it stops the decodes first.
		std::unique_ptr<thumbnail_service> m_thumbnails;
		// Main thread only. A job takes the cache it is started with (never this member), so what it does after the project
		// closed or the trust was taken back goes to a closed cache and is dropped.
		std::shared_ptr<thumbnail_disk_cache> m_thumbnail_cache;
		std::filesystem::path m_thumbnail_cache_root;
		// A prune job runs: set by the job itself when it ends (a cancelled job never reports back), so it also lapses by time.
		std::shared_ptr<std::atomic<bool>> m_thumbnail_prune_running;
		double m_thumbnail_prune_started = 0;
		ds::lifetime m_thumbnail_jobs{ ds::lifetime_option::cancel_on_destroy };
	};
}
