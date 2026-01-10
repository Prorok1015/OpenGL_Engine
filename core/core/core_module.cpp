#include "core_module.h"
#include "res_resource_service_init.h"
#include "wnd_window_service_init.h"
#include "desc_desc_service_init.h"
#include "ecs_ecs_service_init.h"
#include "wnd_window_system.h"
#include "res_system.h"
#include "adapters/res_adapter_worker.h"

void core::core_module::register_services(ds::app_data_storage& data)
{
	res::resource_init(data);// core
	desc::desc_init(data);// core
	window::window_init(data);// core
	ecs::ecs_init(data);// core
}

void core::core_module::initialize_services(ds::app_data_storage& data)
{
	// Initialize core services here
	auto& window = data.require<wnd::window_system>();
	window.init();
	auto& resource = data.require<::res::resource_system>();
	resource.set_adapter_worker(std::make_unique<core::res::async_adapter_worker>());
}

void core::core_module::shutdown_services(ds::app_data_storage& data)
{
	ecs::ecs_term(data);
	window::window_term(data);
	desc::desc_term(data);
	res::resource_term(data);
}
