#include "gui_init.h"
#include "gui_system.h"

extern gui::gui_system* p_gui_system;

void components::gui_init(ds::app_data_storage& data)
{
	p_gui_system = &data.construct<gui::gui_system>();
}

void components::gui_term(ds::app_data_storage& data)
{
	data.destruct<gui::gui_system>();
	p_gui_system = nullptr;
}
