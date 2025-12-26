#include "gui_gui_service_init.h"
#include "gui_system.h"
#include "wnd_window_system.h"

extern gui::gui_system* p_gui_system;

void engine::gui::gui_init(ds::app_data_storage& data)
{
	auto& wnd_system = data.require<wnd::window_system>();// TODO: get backend directly from data storage
	p_gui_system = &data.construct<::gui::gui_system>(wnd_system.get_gui_backend());
}

void engine::gui::gui_term(ds::app_data_storage& data)
{
	data.destruct<::gui::gui_system>();
	p_gui_system = nullptr;
}
