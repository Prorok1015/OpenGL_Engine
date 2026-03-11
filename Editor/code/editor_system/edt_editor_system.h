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
#include "scn_renderer.h"
#include "edt_editor_layer.h"
#include "level/scn_level_manager.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_viewport_panel.h"
#include "edt_console_panel.h"
#include "edt_asset_browser_panel.h"

namespace edt
{
	class editor_test_field_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;

		editor_test_field_desc() = default;
		virtual ~editor_test_field_desc() = default;

		virtual void copy_to(desc_base& other) const override
		{
			editor_test_field_desc& other_desc = static_cast<editor_test_field_desc&>(other);
			other_desc = *this;
		}

		virtual void deserialize(desc::desc_system& desc_system, const json::object& data) override
		{
			/*if (data.contains("__parent")) {
				res::tag parent_tag{ data.at("__parent").as_string() };
				*this = *desc_system.get_desc<editor_test_field_desc>(parent_tag);
			}*/

			if (data.contains("field_string"))
				field_string = data.at("field_string").as_string();
		}

		virtual void serialize(json::object& data) const override
		{
		}

		std::string field_string;
	};

	class editor_test_sub_field_desc : public editor_test_field_desc
	{
		using base_type = editor_test_field_desc::base_type;
	};

	class editor_test_parent_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;
		editor_test_parent_desc() = default;
		virtual ~editor_test_parent_desc() = default;
		virtual void deserialize(desc::desc_system& res_system, const json::object& data) override
		{
			if (data.contains("just_number"))
				just_number = data.at("just_number").as_int64();
			if (data.contains("just_double"))
				just_double = data.at("just_double").as_double();
		}

		virtual void serialize(json::object& data) const override
		{
		}

		virtual void copy_to(desc_base& other) const override
		{
			editor_test_parent_desc& other_desc = static_cast<editor_test_parent_desc&>(other);
			other_desc = *this;
		}

		int just_number = 0;
		double just_double = 0.0;
	};

	class editor_test_desc : public editor_test_parent_desc
	{
	public:
		using base_type = editor_test_parent_desc::base_type;
		editor_test_desc() = default;
		virtual ~editor_test_desc() = default;
		virtual void deserialize(desc::desc_system& desc_system, const json::object& data) override
		{
			editor_test_parent_desc::deserialize(desc_system, data);

			if (data.contains("just_string"))
				just_string = data.at("just_string").as_string();

			if (data.contains("field")) {
				field = desc_system.get_field_desc<editor_test_field_desc>(*this, data.at("field"));
			}
		}

		std::string just_string;
		res::res_handle<editor_test_field_desc> field;
	};

	struct crossing_context
	{
		struct direction_hint
		{
			float aircraft_speed = 10.f;
			float ship_speed = 5.f;
			int nearest_ship_index = -1;
			float nearest_ship_distance = std::numeric_limits<float>::max();
		};
		direction_hint hint;
		glm::vec2 aircraft;
		glm::vec2 aircraft_dir;
		ds::fixed_vector<glm::vec2, 10> ships;
		ds::fixed_vector<glm::vec2, 10> ships_dirs;
		ds::fixed_vector<bool, 10> alive_ships;
		bool is_initialized = false;
		bool is_paused = true;
		bool is_aircraft_placed = false;
	};

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
		bool show_crossing_game_window();
		bool show_web();
		bool show_scene();
		bool show_ecs_test();
		bool show_textures();
		void show_tree_items(ecs::entity ent);
		bool show_clear_cache();
		void draw_manipulator(const glm::vec2& pos, const glm::vec2& size);
		void draw_gizmo(const glm::vec2& pos, const glm::vec2& size, const glm::mat4& view, const glm::mat4& proj);
		void draw_scene_image(const glm::vec2& pos, const glm::vec2& contentRegionAvailable);
		bool init_ecs_test();
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

		ecs::entity test_json_selected_material = entt::null;
		ecs::entity selected_entity = entt::null;

		bool is_show_web = true;
		bool is_inited_ecs_test = false;
		std::shared_ptr<edt::input_manager> input;
		std::shared_ptr<inp::ecs_input_manager> ecs_input;
		edt::file_dialog file_dialog;
		
		std::shared_ptr< scn::skinning_prototype_desc> backpack;
		ds::rtree_q<uint32_t, ds::bbox> rtree;
		desc::desc_system& desc_system;
		std::weak_ptr<scn::level_manager> m_lvl_manager;
		crossing_context crossingcontext;
		std::shared_ptr<entt::registry> registry_sp;
		std::shared_ptr<scn::renderer_3d> renderer_sp;
		std::shared_ptr<edt::editor_layer> editor_layer;

		std::shared_ptr<scene_hierarchy_panel> m_hierarchy_panel;
		std::shared_ptr<inspector_panel>       m_inspector_panel;
		std::shared_ptr<viewport_panel>        m_viewport_panel;
		std::shared_ptr<console_panel>         m_console_panel;
		std::shared_ptr<asset_browser_panel>   m_asset_browser_panel;
	};


}
