#include "engine_module.h"
#include "rnd_render_service_init.h"
#include "gs_game_init.h"
#include "gui_gui_service_init.h"
#include "inp_input_service_init.h"
#include "inp_input_system.h"
#include "wnd_window_system.h"
#include "scn_scene_service_init.h"
#include "app_loop_service_interface.h"

void engine::engine_module::register_services(ds::app_data_storage& data)
{
	input::input_reg(data);// engine
	render::render_init(data);// engine
	gui::gui_init(data);//engine
	scn::scene_init(data);//engine
	game::game_init(data);//delete
}

void engine::engine_module::initialize_services(ds::app_data_storage& data)
{
	// Initialize engine services here
	data.require<app::app_loop_service_interface>().init(data);

	auto input_service = data.require_shared<inp::input_system>();
	auto& win_service = data.require<wnd::window_system>();
	if (input_service) {
		win_service.add_event_listener(input_service);
		// HACK! first init window size in input system. 
		// Because window created in window system constructor before input system subscribe
		auto window = win_service.get_active_window();
		input_service->on_window_created(window.get());
		input_service->on_window_resize(window.get(), window->get_size().x, window->get_size().y);
	}
	input::input_init(data);
}

void engine::engine_module::shutdown_services(ds::app_data_storage& data)
{
	auto input_service = data.require_shared<inp::input_system>();
	if (input_service) {
		auto& win_service = data.require<wnd::window_system>();
		win_service.remove_event_listener(input_service);
	}

	game::game_term(data);
	scn::scene_term(data);
	gui::gui_term(data);
	render::render_term(data);
	input::input_term(data);
}