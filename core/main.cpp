#include <application.h>
#include <eng_module_loader.hpp>
#include <ds_store.hpp>
#include <core_module.h>
#include <engine_module.h>

int main()
{
	ds::app_data_storage app_storage;

	modules::module_loader module_loader;
	module_loader.add_module(std::make_unique<core::core_module>());
	module_loader.add_module(std::make_unique<engine::engine_module>());

	module_loader.register_all_services(app_storage);
	module_loader.initialize_all_services(app_storage);

	app::application& myApp = app::get_app_system();
	int result = myApp.run(app_storage);

	module_loader.shutdown_all_services(app_storage);
	return result;
}