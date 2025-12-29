#include "core_module.h"
#include "res_resource_service_init.h"
#include "wnd_window_service_init.h"
#include "desc_desc_service_init.h"
#include "wnd_window_system.h"

void core::core_module::register_services(ds::app_data_storage& data)
{
	res::resource_init(data);// core
	desc::desc_init(data);// core
	window::window_init(data);// core
}

void core::core_module::initialize_services(ds::app_data_storage& data)
{
	// Initialize core services here
	auto& window = data.require<wnd::window_system>();
	window.init();
}

void core::core_module::shutdown_services(ds::app_data_storage& data)
{
	window::window_term(data);
	desc::desc_term(data);
	res::resource_term(data);
}
