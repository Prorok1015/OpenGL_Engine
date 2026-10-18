#include <snk/application/application.h>
#include <snk/application/app_loop_service_interface.h>
#include <exception>
#include <snk/common/logger/engine_log.h>

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
	// Every module is initialized by now, so the loop may rely on any service it registered.
	try {
		// Inside the guard: a loop that failed halfway through init() still gets its term().
		app_loop_service.init(storage);
		while (!app_loop_service.should_stop())
		{
			app_loop_service.on_step(storage);
		}
	} catch (...) {
		// The loop was initialized, so it is terminated even when a step fails; the step's
		// exception is the one that propagates.
		try {
			app_loop_service.term(storage);
		} catch (const std::exception& e) {
			egLOG_ERROR("app", "loop term failed after a step error: {}", e.what());
		} catch (...) {
			egLOG_ERROR("app", "loop term failed after a step error");
		}
		throw;
	}
	app_loop_service.term(storage);

	return 0;
}