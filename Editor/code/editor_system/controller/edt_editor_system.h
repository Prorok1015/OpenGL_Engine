#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <snk/resource/resources/res_resource_handle.hpp>
#include "edt_editor_camera.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_editor_services.h>
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "edt_level_controller.h"
#include "edt_async_import_task.h"
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/logger/engine_log.h>
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

namespace edt {
	class agent_server;
	class file_menu;
	class editor_context;
	class editor_layer;
	class model_importer;
	class asset_exporter;
	class asset_export_dialog;
	class scene_hierarchy_panel;
	class inspector_panel;
	class viewport_panel;
	class game_view_panel;
	class console_panel;
	class blackboard_panel;
	class asset_browser_panel;
	class prefab_editor_panel;
	class prefab_document;

	class editor_system
	{
	public:
		editor_system(desc::desc_system& desc_system, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys);
		// Closes the prefab preview and releases the scene document in the editor_context before its members die.
		~editor_system();

		void init(ds::app_data_storage& inp_sys);

		// Created in init() and owned by the app_data_storage; null before init().
		editor_context* context() const { return m_context; }

		void begin_level_load(const res::tag& level_tag);

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

		// Services (not owned)
		desc::desc_system&    desc_system;
		res::resource_system& m_res;
		rnd::render_system&   m_rnd;
		gui::gui_system&      m_gui;

		// Input
		inp::input_router* m_input_router = nullptr;

		// Import/export
		std::unique_ptr<edt::model_importer> m_model_importer;
		std::unique_ptr<edt::asset_exporter> m_asset_exporter;
		std::unique_ptr<edt::asset_export_dialog> m_export_dialog;
		// The import in flight (m_importing); the job's on_done fills m_import_result or m_import_failure on the main thread.
		bool m_importing = false;
		bool m_import_finished = false;
		std::uint64_t m_import_job = 0;
		std::string m_import_name;
		float m_import_spinner_t = 0.f;
		std::optional<import_intermediate> m_import_result;
		// Set when the job itself failed (exception); a failed import reports through m_import_result->error.
		std::string m_import_failure;
		// Owner of the job callbacks; after the import state and the importer/dialog they touch, so it dies (and cancels) first.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
		void start_import(const std::filesystem::path& abs_path);
		bool poll_import_pipeline();
		bool poll_import_progress();
		void finish_import();

		// init() steps: the panels, their dependencies and the callbacks between panels and controllers.
		void create_panels(bool capture_log);
		void create_viewport_panel();
		void wire_controller_callbacks();
		void wire_panel_callbacks();

		// Editor layer & panels
		std::shared_ptr<edt::editor_layer> editor_layer;

		std::shared_ptr<scene_hierarchy_panel> m_hierarchy_panel;
		std::shared_ptr<inspector_panel>       m_inspector_panel;
		std::shared_ptr<viewport_panel>        m_viewport_panel;
		std::shared_ptr<game_view_panel>       m_game_view_panel;
		std::shared_ptr<console_panel>         m_console_panel;
		std::shared_ptr<blackboard_panel>      m_blackboard_panel;
		// The console's feed from the engine log; removed with the editor system.
		engine::log_sink_handle                m_log_sink;
		std::shared_ptr<asset_browser_panel>   m_asset_browser_panel;
		std::shared_ptr<prefab_editor_panel>   m_prefab_panel;

		// Prefab editor (EPIC-25); the preview document itself is owned by the editor_context's document_manager
		void open_prefab_for_edit(const res::tag& tag);
		void close_prefab_editor();

		// Async level load (US-24-4)
		res::res_handle<scn::level_desc> m_loading_level_handle;
		bool m_loading_level = false;
		bool poll_level_load();

		// Async asset drop / export auto-add (US-24-5)
		res::res_handle<desc::desc_base> m_pending_desc_handle;
		res::tag m_pending_desc_tag;
		std::string m_pending_desc_key;
		bool poll_pending_desc();

		bool m_export_dialog_active = false;
		std::function<void()> m_exit_action;
		std::unique_ptr<edt::file_menu> m_file_menu;

		// Selection routing between the panels; last member, so it is dropped before the panels are.
		std::vector<connection> m_selection_connections;
		// Panels and menus in the editor_context registries; removed first, before the panels they point at.
		std::vector<registration> m_registrations;

		// The MCP server (editor/agent_server, off by default); null when it is off. Last member: stopped first.
		std::unique_ptr<agent_server> m_agent_server;
	};
}
