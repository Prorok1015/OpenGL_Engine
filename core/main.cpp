#include <application.h>
#include <components_init.h>

int main()
{
	ds::app_data_storage app_storage;
	com::component_init(app_storage);

	app::application& myApp = app::get_app_system();
	int result = myApp.run(app_storage);

	com::component_term(app_storage);

	return result;
}