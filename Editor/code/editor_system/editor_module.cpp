#include "editor_module.h"
#include "edt_editor_init.h"

void editor::editor_module::register_services(ds::app_data_storage& data)
{
	using namespace components;
	//4
	editor_init(data);// editor
}

void editor::editor_module::initialize_services(ds::app_data_storage& data)
{
	// Initialize editor services here
}

void editor::editor_module::shutdown_services(ds::app_data_storage& data)
{
	using namespace components;
	editor_term(data);
}