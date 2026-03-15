#pragma once
#include "common.h"
#include "scn_model.h"
#include "ecs_entity.h"
#include "rnd_render_system.h"
#include "edt_input_manager.h"
#include "inp_ecs_input_manager.h"
#include "inp_input_system.h"
#include "edt_file_dialog.h"

#include "desc_base.hpp"
#include "desc_system.h"
#include "scn_skinning_prototype_desc.h"
#include "ds/ds_rtree.h"
#include "ds/ds_store.hpp"

#include "edt_editor_layer.h"
#include "level/scn_level_manager.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_viewport_panel.h"
#include "edt_console_panel.h"
#include "edt_asset_browser_panel.h"

namespace edt {
	class editor_system
	{
	public:
		editor_system(desc::desc_system& desc_system);
		~editor_system();

		void init(ds::app_data_storage& inp_sys);

		bool show_toolbar();
		bool show_file_dialog();

		//TODO change to renderer
		bool load_level();
		bool show_web();
		bool show_scene();
		void show_tree_items(ecs::entity ent);
		bool show_clear_cache();
		void draw_gizmo(const glm::vec2& pos, const glm::vec2& size, const glm::mat4& view, const glm::mat4& proj);
		void draw_scene_image(const glm::vec2& pos, const glm::vec2& contentRegionAvailable);
	private:
		std::vector<std::string> cameras_list
		{
			"Main", "Second"
		};
		int current_camera = 0;

		std::vector<std::string> render_modes_list
		{
			"TRIANGLE", "TRIANGLE_STRIP", "LINE_LOOP", "LINE_STRIP", "LINE", "POINT"
		};

		std::unordered_map<std::string, rnd::RENDER_MODE> mmap{
				{render_modes_list[0], rnd::RENDER_MODE::TRIANGLE},
				{render_modes_list[1], rnd::RENDER_MODE::TRIANGLE_STRIP},
				{render_modes_list[2], rnd::RENDER_MODE::LINE_LOOP},
				{render_modes_list[3], rnd::RENDER_MODE::LINE_STRIP},
				{render_modes_list[4], rnd::RENDER_MODE::LINE},
				{render_modes_list[5], rnd::RENDER_MODE::POINT},
		};
		int current_render_mode = 0;

		std::vector<res::tag> imported_models_list;

		std::vector<std::string> models_list
		{
			"objects/anim_cube_f/tail_cube.glb",
			"objects/robot/gen_robot.glb",
			"objects/anim_cube_f/bird_cube.glb",
			"objects/anim_cube_f/jump_cube.glb",
			"objects/anim_cube_f/anim_cube.glb",
			"objects/getaur/scene.gltf",
			"objects/train/scene.gltf",
			"objects/fsb/scene.gltf",
			"objects/backpack/backpack.obj",
			"objects/luke/Luke_01.fbx",
			"objects/flower/source/flower.fbx",
			"objects/helicopter/source/helicopter Space ship.glb",
			"objects/Cheeseburger.glb",
		};
		int current_model = 1;

		char buf[64] = "objects/";

		glm::vec4 clear_color {1};
		ecs::entity editor_web;
		ecs::entity light;
		ecs::entity sky;
		ecs::entity world_anchor;
		ecs::entity backpackent;

		ecs::entity selected_entity = entt::null;

		bool is_show_web = true;
		std::shared_ptr<edt::input_manager> input;
		std::shared_ptr<inp::ecs_input_manager> ecs_input;
		edt::file_dialog file_dialog;
		
		std::shared_ptr< scn::skinning_prototype_desc> backpack;
		ds::rtree_q<uint32_t, ds::bbox> rtree;
		desc::desc_system& desc_system;
		std::weak_ptr<scn::level_manager> m_lvl_manager;
		std::shared_ptr<entt::registry> registry_sp;

		std::shared_ptr<edt::editor_layer> editor_layer;

		std::shared_ptr<scene_hierarchy_panel> m_hierarchy_panel;
		std::shared_ptr<inspector_panel>       m_inspector_panel;
		std::shared_ptr<viewport_panel>        m_viewport_panel;
		std::shared_ptr<console_panel>         m_console_panel;
		std::shared_ptr<asset_browser_panel>   m_asset_browser_panel;

		// Scene Save/Load
		void new_level();
		bool save_level();

		res::tag                 m_level_tag;
		std::string              m_world_name;
		std::vector<std::string> m_world_systems;
		bool                     m_save_dialog_open = false;
		char                     m_save_path_buf[512] = {};
	};


}
