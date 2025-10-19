#include "wnd_window_init.h"
#include "wnd_window_system.h"

extern wnd::window_system* p_wnd_system;

void components::window_init(ds::AppDataStorage& data)
{
	p_wnd_system = &data.construct<wnd::window_system>();
}

void components::window_term(ds::AppDataStorage& data)
{

	data.destruct<wnd::window_system>();
	p_wnd_system = nullptr;
}
