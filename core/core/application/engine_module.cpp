#include "engine_module.h"
#include "rnd_render_init.h"
#include "gs_game_init.h"
#include "gui_init.h"

void engine::engine_module::register_services(ds::app_data_storage& data)
{
	using namespace components;
	//3
	render_init(data);// engine
	gui_init(data);//engine
	game_init(data);//delete
}

void engine::engine_module::initialize_services(ds::app_data_storage& data)
{
	// Initialize engine services here
}

void engine::engine_module::shutdown_services(ds::app_data_storage& data)
{
	using namespace components;
	game_term(data);
	gui_term(data);
	render_term(data);
}