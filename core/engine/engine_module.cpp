#include "engine_module.h"
#include "rnd_render_init.h"
#include "gs_game_init.h"
#include "gui_init.h"
#include "inp_input_init.h"
#include "inp_input_system.h"
#include "wnd_window_system.h"
#include "gs_loop_service.h"

void engine::engine_module::register_services(ds::app_data_storage& data)
{
	using namespace components;
	//3
	input_init(data);// engine
	render_init(data);// engine
	gui_init(data);//engine
	game_init(data);//delete
	data.construct<app::app_loop_service_interface, gs::gs_loop_service>();
}

void engine::engine_module::initialize_services(ds::app_data_storage& data)
{
	// Initialize engine services here
	data.require<app::app_loop_service_interface>().init(data);

	auto input_service = data.require_shared<inp::input_system>();
	if (input_service) {
		auto& win_service = data.require<wnd::window_system>();
		win_service.add_event_listener(input_service);
		// hack! to init window size in input system
		input_service->on_window_resize(win_service.get_active_window().get(), win_service.get_active_window()->get_size().x, win_service.get_active_window()->get_size().y);
	}
}

void engine::engine_module::shutdown_services(ds::app_data_storage& data)
{
	using namespace components;
	auto input_service = data.require_shared<inp::input_system>();
	if (input_service) {
		auto& win_service = data.require<wnd::window_system>();
		win_service.remove_event_listener(input_service);
	}

	data.destruct<app::app_loop_service_interface>();
	game_term(data);
	gui_term(data);
	render_term(data);
	input_term(data);
}