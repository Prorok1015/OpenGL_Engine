#include "wnd_window_init.h"
#include "wnd_window_system.h"

void components::window_init(ds::app_data_storage& data)
{
	data.construct<wnd::window_system>();
}

void components::window_term(ds::app_data_storage& data)
{
	data.destruct<wnd::window_system>();
}
