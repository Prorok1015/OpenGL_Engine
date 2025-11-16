#include "application.h"
#include "app_loop_service_interface.h"

app::application* p_app_system = nullptr;

app::application& app::get_app_system()
{
	ASSERT_MSG(p_app_system, "application system is nullptr!");
	return *p_app_system;
}

app::application::application()
{
}

app::application::~application()
{
}

int app::application::run(ds::app_data_storage& storage)
{
	if (!storage.has_value<app::app_loop_service_interface>()) {
		return -1;
	}

	auto& app_loop_service = storage.require<app::app_loop_service_interface>();
	while (!app_loop_service.should_stop())
	{
		app_loop_service.on_step(storage);
	}

	return 0;
}