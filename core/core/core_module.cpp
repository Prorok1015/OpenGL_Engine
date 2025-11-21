#include "core_module.h"
#include "res_module_init.h"
#include "wnd_window_init.h"
#include "desc_init.h"
#include "application.h"

extern app::application* p_app_system;

void core::core_module::register_services(ds::app_data_storage& data)
{
	using namespace components;
	p_app_system = &data.construct<app::application>();

	//1
	resource_init(data);// core
	desc_init(data);// core
	//2
	window_init(data);// core
}

void core::core_module::initialize_services(ds::app_data_storage& data)
{
	// Initialize core services here
}

void core::core_module::shutdown_services(ds::app_data_storage& data)
{
	using namespace components;
	window_term(data);

	desc_term(data);
	resource_term(data);

	data.destruct<app::application>();
	p_app_system = nullptr;
}
