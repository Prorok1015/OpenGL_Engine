#include "inp_input_init.h"
#include "inp_input_system.h"

extern inp::input_system* p_inp_system;

void components::input_init(ds::app_data_storage& data)
{
	p_inp_system = &data.construct<inp::input_system>();
}

void components::input_term(ds::app_data_storage& data)
{
	data.destruct<inp::input_system>();
	p_inp_system = nullptr;
}
