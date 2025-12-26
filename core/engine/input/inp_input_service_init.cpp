#include "inp_input_service_init.h"
#include "inp_input_system.h"

void engine::input::input_init(ds::app_data_storage& data)
{
	data.construct<inp::input_system>();
}

void engine::input::input_term(ds::app_data_storage& data)
{
	data.destruct<inp::input_system>();
}
