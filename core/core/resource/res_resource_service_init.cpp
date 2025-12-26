#include "res_resource_service_init.h"
#include "res_system.h"

extern res::resource_system* p_res_system;

void core::resource::resource_init(ds::app_data_storage& data)
{
	p_res_system = &data.construct<res::resource_system>();
}

void core::resource::resource_term(ds::app_data_storage& data)
{
	data.destruct<res::resource_system>();
	p_res_system = nullptr;
}