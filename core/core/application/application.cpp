#include "application.h"
#include "app_loop_service_interface.h"

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