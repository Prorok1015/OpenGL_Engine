#pragma once
#include <functional>
#include <memory>
#include "resources/res_resource_handle.hpp"
#include "edt_editor_camera.h"
#include "edt_shared_state.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "edt_level_controller.h"
#include "edt_init_helpers.h"
#include "ds/ds_store.hpp"

// Forward declarations — full types only needed in .cpp
namespace desc { class desc_system; class desc_base; }
namespace res  { class resource_system; }
namespace rnd  { class render_system; }
namespace gui  { class gui_system; }
namespace scn  { class ecs_assembler; class level_desc; }
namespace ecs  { class system_factory; }

namespace edt {
	class input_manager;
	class file_dialog;
	class editor_layer;
	class model_importer;
	class asset_exporter;
	class asset_export_dialog;
	class component_ui_registry;
	class scene_hierarchy_panel;
	class inspector_panel;
	class viewport_panel;
	class console_panel;
	class asset_browser_panel;
	struct async_import_token;

	class editor_system
	{
		friend void init_helpers::wire_controller_callbacks(
			editor_system&, scene_editor&, world_controller&, level_controller&,
			editor_layer&, scene_hierarchy_panel&, inspector_panel&, viewport_panel&, shared_state&);
		friend void init_helpers::setup_viewport_panel(
			std::shared_ptr<viewport_panel>&, std::shared_ptr<input_manager>&,
			std::shared_ptr<inp::ecs_input_manager>&, editor_system&, shared_state&,
			rnd::render_system&, gui::gui_system&, res::resource_system&);
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
		desc::desc_system&    desc_system;
		res::resource_system& m_res;
		rnd::render_system&   m_rnd;
		gui::gui_system&      m_gui;

		// Input
		std::shared_ptr<edt::input_manager> input;
		std::shared_ptr<inp::ecs_input_manager> ecs_input;
		std::unique_ptr<edt::file_dialog> m_file_dialog;

		// Import/export
		std::unique_ptr<edt::model_importer> m_model_importer;
		std::unique_ptr<edt::asset_exporter> m_asset_exporter;
		std::unique_ptr<edt::asset_export_dialog> m_export_dialog;
		std::vector<res::tag> imported_models_list;
		std::unique_ptr<edt::async_import_token> m_import_token;
		bool poll_import_progress();

		// Editor layer & panels
		std::shared_ptr<edt::editor_layer> editor_layer;
		std::unique_ptr<edt::component_ui_registry> m_component_ui_registry;

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

	};
}
