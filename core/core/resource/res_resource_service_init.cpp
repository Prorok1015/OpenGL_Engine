#include <snk/resource/res_resource_service_init.h>
#include <snk/resource/res_system.h>

void core::res::resource_init(ds::app_data_storage& data)
{
	data.construct<::res::resource_system>();
}

void core::res::resource_term(ds::app_data_storage& data)
{
	data.destruct<::res::resource_system>();
}