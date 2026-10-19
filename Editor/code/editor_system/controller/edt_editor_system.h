#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <snk/resource/resources/res_resource_handle.hpp>
#include "edt_editor_camera.h"
#include "edt_shared_state.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "edt_level_controller.h"
#include "edt_init_helpers.h"
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/ds/ds_task_system.hpp>
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
	class asset_browser_panel;
	class prefab_editor_panel;
	class prefab_editor_context;
	struct async_import_token;

	class editor_system
	{
		friend void init_helpers::wire_controller_callbacks(
			editor_system&, scene_editor&, world_controller&, level_controller&,
			editor_layer&, shared_state&);
		friend void init_helpers::setup_viewport_panel(
			std::shared_ptr<viewport_panel>&, inp::input_router&, editor_system&, shared_state&,
			rnd::render_system&, gui::gui_system&, res::resource_system&);
	public:
		editor_system(desc::desc_system& desc_system, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys);
		~editor_system();

		void init(ds::app_data_storage& inp_sys);

		// Created in init() and owned by the app_data_storage; null before init().
		editor_context* context() const { return m_context; }

		void begin_level_load(const res::tag& level_tag);

	private:
		editor_context* m_context = nullptr;

		// Shared state for all controllers
		shared_state m_state;

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
		std::unique_ptr<edt::async_import_token> m_import_token;
		// Owner of the job callbacks; after m_import_token and the importer/dialog they touch, so it dies (and cancels) first.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
		void start_import(const std::filesystem::path& abs_path);
		bool poll_import_pipeline();
		bool poll_import_progress();

		// Editor layer & panels
		std::shared_ptr<edt::editor_layer> editor_layer;

		std::shared_ptr<scene_hierarchy_panel> m_hierarchy_panel;
		std::shared_ptr<inspector_panel>       m_inspector_panel;
		std::shared_ptr<viewport_panel>        m_viewport_panel;
		std::shared_ptr<game_view_panel>       m_game_view_panel;
		std::shared_ptr<console_panel>         m_console_panel;
		std::shared_ptr<asset_browser_panel>   m_asset_browser_panel;
		std::shared_ptr<prefab_editor_panel>   m_prefab_panel;

		// Prefab editor (EPIC-25)
		std::unique_ptr<prefab_editor_context> m_prefab_ctx;
		void open_prefab_for_edit(const res::tag& tag);
		void close_prefab_editor();

		// Editor camera (EPIC-11)
		editor_camera_state m_camera_state;
		void save_editor_camera_state(entt::registry& reg);
		void inject_editor_camera(entt::registry& reg);

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

	};
}
