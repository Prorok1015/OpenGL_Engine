#pragma once
#include "common.h"
#include <future>

#include "scn_renderer.h"
#include "wnd_window.h"
#include "scn_camera_controller_system.h"
#include "ecs_input_manager.h"
#include "inp_input_manager.h"
#include "res_instance.h"
#include "desc_system.h"

namespace gs 
{
	class GameSystem
	{
	public:
		GameSystem(desc::desc_system& d);
		~GameSystem();
		GameSystem(const GameSystem&) = delete;
		GameSystem& operator= (const GameSystem&) = delete;
		GameSystem(GameSystem&&) = delete;
		GameSystem& operator= (GameSystem&&) = delete;

		void set_enable_input(bool enable);

		std::shared_ptr<wnd::window> get_window() { return window; };
		void load_model(std::string_view path);
		void ensure_ecs_material(ecs::entity material, const res::Material& mlt);
		void reload_shaders();

		void add_cube_to_scene(float radius);
		void remove_cube();

		void check_loaded_model();
		void end_ecs_frame();

		std::shared_ptr<scn::renderer_3d> get_renderer() const { return renderer; }
		//std::shared_ptr<inp::input_manager> get_input_manager() const { return input; }

		ecs::entity cubes_inst;

	private:
		desc::desc_system& desc_system;
		std::shared_ptr<scn::renderer_3d> renderer;

		std::shared_ptr<wnd::window> window;
		std::shared_ptr<inp::input_manager> input;
		std::shared_ptr<ecs::flow_input_manager> ecs_input;

		std::shared_ptr<std::future<std::shared_ptr<res::Model>>> future_model;
	};

	GameSystem& get_system();

}