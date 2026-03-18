#pragma once
#include <functional>
#include <optional>
#include "common.h"
#include "ecs_entity.h"
#include "edt_input_manager.h"
#include "inp_ecs_input_manager.h"
#include "edt_file_dialog.h"

#include "desc_base.hpp"
#include "desc_system.h"
#include "ds/ds_store.hpp"
#include "res_system.h"
#include <boost/json.hpp>

#include "edt_editor_layer.h"
#include "edt_editor_camera.h"
#include "edt_shared_state.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "edt_level_controller.h"

#include "level/scn_level_manager.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_viewport_panel.h"
#include "edt_console_panel.h"
#include "edt_asset_browser_panel.h"
#include "edt_component_ui_registry.h"
#include "edt_model_importer.h"
#include "edt_asset_exporter.h"
#include "edt_asset_export_dialog.h"
#include "edt_async_import_task.h"
#include "level/scn_level_desc.h"

namespace scn { class ecs_assembler; }
namespace ecs { class system_factory; }
namespace rnd { class render_system; }
namespace gui { class gui_system; }

namespace edt {
	class editor_system
	{
	public:
		editor_system(desc::desc_system& desc_system, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys);
		~editor_system();

		void init(ds::app_data_storage& inp_sys);

		bool show_file_dialog();

		//TODO change to renderer
		bool load_level();

	private:
		// Shared state for all controllers
		shared_state m_state;

		// Controllers
		scene_editor m_scene_editor;
		world_controller m_world_ctrl;
		level_controller m_level_ctrl;

		// Services (not owned)
		desc::desc_system&   desc_system;
		res::resource_system& m_res;
		rnd::render_system&   m_rnd;
		gui::gui_system&      m_gui;

		// Input
		std::shared_ptr<edt::input_manager> input;
		std::shared_ptr<inp::ecs_input_manager> ecs_input;
		edt::file_dialog file_dialog;

		// Import/export
		edt::model_importer m_model_importer;
		edt::asset_exporter m_asset_exporter;
		edt::asset_export_dialog m_export_dialog;
		std::vector<res::tag> imported_models_list;
		std::optional<edt::async_import_token> m_import_token;
		bool poll_import_progress();

		// Editor layer & panels
		std::shared_ptr<edt::editor_layer> editor_layer;
		edt::component_ui_registry m_component_ui_registry;

		std::shared_ptr<scene_hierarchy_panel> m_hierarchy_panel;
		std::shared_ptr<inspector_panel>       m_inspector_panel;
		std::shared_ptr<viewport_panel>        m_viewport_panel;
		std::shared_ptr<console_panel>         m_console_panel;
		std::shared_ptr<asset_browser_panel>   m_asset_browser_panel;

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

		// Panel selection clearing helper
		void clear_selection();
		void sync_viewport_selection();
	};
}
