#include "res_module_init.h"
#include "res_system.h"

extern res::resource_system* p_res_system;

void components::resource_init(ds::app_data_storage& data)
{
	p_res_system = &data.construct<res::resource_system>();
}

void components::resource_term(ds::app_data_storage& data)
{
	data.destruct<res::resource_system>();
	p_res_system = nullptr;
}