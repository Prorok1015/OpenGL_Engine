#pragma once
#include <functional>
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
#include "level/scn_level_manager.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_viewport_panel.h"
#include "edt_console_panel.h"
#include "edt_asset_browser_panel.h"

namespace scn { class ecs_assembler; }
namespace ecs { class system_factory; }
namespace res { class resource_system; }
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
		std::vector<res::tag> imported_models_list;

		std::shared_ptr<edt::input_manager> input;
		std::shared_ptr<inp::ecs_input_manager> ecs_input;
		edt::file_dialog file_dialog;
		desc::desc_system&   desc_system;
		res::resource_system& m_res;
		rnd::render_system&   m_rnd;
		gui::gui_system&      m_gui;
		std::weak_ptr<scn::level_manager> m_lvl_manager;
		scn::ecs_assembler*     m_assembler = nullptr;
		ecs::system_factory*    m_sfactory  = nullptr;

		std::shared_ptr<edt::editor_layer> editor_layer;

		std::shared_ptr<scene_hierarchy_panel> m_hierarchy_panel;
		std::shared_ptr<inspector_panel>       m_inspector_panel;
		std::shared_ptr<viewport_panel>        m_viewport_panel;
		std::shared_ptr<console_panel>         m_console_panel;
		std::shared_ptr<asset_browser_panel>   m_asset_browser_panel;

		// Multi-world
		void switch_to_world(int idx);
		void create_world(const std::string& name);

		std::vector<std::string> m_world_names;
		int                      m_active_world_idx = 0;

		// Scene Save/Load
		void new_level();
		bool save_level();
		void quick_save_level();
		bool show_exit_confirm();

		res::tag                                   m_editor_tag;
		std::string                                m_level_name;
		std::vector<scn::world_desc>               m_world_descs;
		scn::world_desc& active_world_desc() { return m_world_descs[m_active_world_idx]; }
		const scn::world_desc& active_world_desc() const { return m_world_descs[m_active_world_idx]; }
		void on_editor_world_reloaded();

		// Level serialization — single source of truth for the level JSON schema
		boost::json::object load_desc_template(const std::string& filename);
		boost::json::object build_level_json() const;
		bool                write_level_to_disk(const std::string& rel_path);
		void populate_worlds_from_level(const res::res_handle<scn::level_desc>& level_res);

		// Desc-driven scene editing (EPIC-09)
		void serialize_and_push();
		void create_entity(const std::string& type_name);
		void delete_entity(const std::string& name);
		void duplicate_entity(const std::string& name);
		void on_transform_committed(const std::string& name, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale);
		void sync_ecs_transforms_to_desc();

		// Desc helpers
		std::string make_unique_key(const std::string& base_name) const;
		scn::prefab_desc::prefab_node* find_node_by_name(std::vector<scn::prefab_desc::prefab_node>& nodes, const std::string& name);
		bool remove_node_by_name(std::vector<scn::prefab_desc::prefab_node>& nodes, const std::string& name);
		std::vector<scn::prefab_desc::prefab_node>* find_parent_children(std::vector<scn::prefab_desc::prefab_node>& nodes, const std::string& name);

		// Editor camera (EPIC-11)
		editor_camera_state m_camera_state;
		void save_editor_camera_state(entt::registry& reg);
		void inject_editor_camera(entt::registry& reg);

		res::tag                                   m_level_tag;
		std::vector<std::vector<std::string>>      m_worlds_systems_list;
		bool                                       m_save_dialog_open = false;
		char                                       m_save_path_buf[512] = {};
		bool                                       m_is_dirty = false;
		std::function<void()>                      m_exit_action;
		std::function<void()>                      m_confirm_action;
	};


}
