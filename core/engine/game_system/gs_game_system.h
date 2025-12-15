#pragma once
#include "common.h"
#include <future>
#include "scn_renderer.h"
#include "wnd_window.h"
#include "scn_camera_controller_system.h"
#include "ecs_input_manager.h"
#include "desc_system.h"

namespace gs 
{
	class game_system
	{
	public:
		game_system(desc::desc_system& d);
		~game_system();
		game_system(const game_system&) = delete;
		game_system& operator= (const game_system&) = delete;
		game_system(game_system&&) = delete;
		game_system& operator= (game_system&&) = delete;

		void set_enable_input(bool enable);

		void reload_shaders();
		void end_ecs_frame();

		std::shared_ptr<scn::renderer_3d> get_renderer() const { return renderer; }
	private:
		desc::desc_system& desc_system;
		std::shared_ptr<scn::renderer_3d> renderer;
	};

	game_system& get_system();

}