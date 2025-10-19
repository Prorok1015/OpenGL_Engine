#include "inp_input_init.h"
#include "inp_input_system.h"

extern inp::input_system* p_inp_system;

void components::input_init(ds::AppDataStorage& data)
{
	p_inp_system = &data.construct<inp::input_system>();
}

void components::input_term(ds::AppDataStorage& data)
{
	data.destruct<inp::input_system>();
	p_inp_system = nullptr;
}
