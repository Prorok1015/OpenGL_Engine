#pragma once
#include "edt_migration_startup.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_registration.h>
#include <snk/project_migration/pmg_geometry_check_cache.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cmd { struct log_read_filter; }
namespace prj { struct project_context; }
namespace res { class resource_system; }

namespace edt
{
	class asset_open_component;
	class editor_context;
	class editor_layer;
	class level_controller;
	class level_load_component;
	class migration_panel;
	class panel_base;
	class prefab_editor_component;
	class scene_document;
	struct editor_services;

	// Project migration (EPIC-58), whole: project.migration_plan / project.migrate and the rollback commands, the Project Migration
	// window and its action, the offer to roll back an unfinished migration at startup, the notice "N files in an old format" with
	// the scan behind it, and what a running or stopped migration refuses meanwhile (`blocker`, `project_write_blocker`).
	// editor_system creates it after the prefab editor, the asset opening and the level load it closes and reopens, and keeps
	// `migration_blocker()` / `project_write_blocker()` as thin forwarders for the components that ask them.
	class migration_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_services& services;
			res::resource_system& resources;
			editor_layer& layer;
			level_controller& level_ctrl;
			scene_document& state;
			prefab_editor_component& prefab;
			asset_open_component& asset_open;
			level_load_component& level_load;
			// Null without a project.
			const prj::project_context* project;
			// The project is held back for its engine version: nothing of it is offered or opened.
			bool project_held;
			bool headless;
			cmd::log_read_filter* log_filter;
			// Why a migration cannot start now: Play, a load, a build, a reload, an import, the export dialog, a git write. Null: none.
			std::function<std::optional<std::string>()> busy_reason;
			// Why the asset index is not ready yet (asset_index_wait, #1278): the plan waits for it too. Null: ready.
			std::function<std::optional<std::string>()> index_wait;
			// A person is in the middle of an edit, or a document has unsaved changes. Null: none.
			std::function<std::optional<std::string>()> unsaved_reason;
			// Moves with every change of the game layer: project.migrate binds the plan a person approved to the code that runs it.
			std::function<std::uint64_t()> game_layer_generation;
			// The agent policy's confirmations exist only with the agent server: read when asked.
			std::function<bool(std::string_view ticket)> approved_internally;
			// The asset database exists only with a project.
			std::function<void()> rebuild_assets;
			// The start level is still loading in game mode (the editor/game mode controller exists after this component).
			std::function<bool()> startup_pending;
		};

		explicit migration_component(ports from);
		~migration_component();
		migration_component(const migration_component&) = delete;
		migration_component& operator=(const migration_component&) = delete;

		// The commands, then the window (its panel goes to the registry through `add_panel`) and its action, in that order.
		void install(std::vector<registration>& out, const std::function<void(const std::shared_ptr<panel_base>&)>& add_panel);
		// The offer to roll back an unfinished migration, once the start level settled (headless: the warning only, at once).
		void install_startup();
		// --open-migration: the window opens once the project is open and the start level settled.
		void install_open_on_start();

		// Project > Migrate Project... and --open-migration: the window, with a fresh plan.
		void open_window();
		// A migration is running: Play, level changes and saves, builds, module reloads and quitting are refused until it ended.
		std::optional<std::string> blocker() const;
		// What refuses a write into the project: a migration that runs, else one that stopped half way and is not rolled back.
		std::optional<std::string> project_write_blocker();
		// Why a level of the project is not saved (a migration stopped half way and is not rolled back); nullopt: none.
		std::optional<std::string> stopped_reason() { return m_stopped.reason(); }

	private:
		void open_rollback(const std::string& id);
		void offer_rollback(const journal_offer& offer);
		void start_scan(const std::filesystem::path& root);
		bool draw_notice();
		bool start_level_settled() const;

		ports m_ports;
		std::shared_ptr<migration_panel> m_panel;
		// A migration (or rollback) stopped half way: no level of the project is saved until that migration is rolled back,
		// whatever level is loaded meanwhile, so the files stay as the rollback expects them.
		migration_save_block m_stopped;
		// project.migrate writes the project on the io lane (#761).
		bool m_running = false;
		// The notice of a project in an old format (#761) and whether its scan still runs.
		std::unique_ptr<migration_notice_model> m_notice;
		// The editor never changes its project: one cache serves every scan of this session (the notice's and the plans').
		std::shared_ptr<prj::migration::geometry_check_cache> m_geometry_checks = std::make_shared<prj::migration::geometry_check_cache>();
		std::uint64_t m_scan_job = 0;
		// Migration and rollback runs started so far: a format check made before one is stale.
		std::uint64_t m_runs = 0;
		// Owner of the format check's callback: declared after what it touches, so it dies (and cancels) first.
		ds::lifetime m_scan_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};
}
