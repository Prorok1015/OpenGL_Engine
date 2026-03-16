#include "wnd_window_service_init.h"
#include "wnd_window_system.h"

void core::window::window_init(ds::app_data_storage& data)
{
	data.construct<wnd::window_system>();
}

void core::window::window_term(ds::app_data_storage& data)
{
	data.destruct<wnd::window_system>();
}
