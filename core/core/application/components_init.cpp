#include "components_init.h"
#include "application.h"
#include <inp_input_init.h>
#include "res_module_init.h"
#include <rnd_render_init.h>
#include <gs_game_init.h>
#include <gui_init.h>
#include <wnd_window_init.h>
#include "desc_init.h"

extern app::application* p_app_system;

void components::component_init(ds::app_data_storage& data)
{
	p_app_system = &data.construct<app::application>();
	//1
	resource_init(data);// core
	desc_init(data);// core
	input_init(data);// engine
	//2
	window_init(data);// core
	//3
	render_init(data);// engine
	gui_init(data);//engine
	game_init(data);//delete
}

void components::component_term(ds::app_data_storage& data)
{
	game_term(data);
	gui_term(data);
	render_term(data);

	window_term(data);

	input_term(data);
	desc_term(data);
	resource_term(data);

	data.destruct<app::application>();
	p_app_system = nullptr;
}
