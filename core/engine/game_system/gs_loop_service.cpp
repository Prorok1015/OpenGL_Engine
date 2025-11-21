#include "gs_loop_service.h"
#include <rnd_render_system.h>
#include <inp_input_system.h>
#include <ecs_system.h>
#include "ecs_common_system.h"
#include <gs_game_system.h>
#include "wnd_window_system.h"

gs::gs_loop_service::gs_loop_service()
	: stop_requested(false)
{
	for (auto& ptr : ecs::job_base::get_jobs(ecs::job_base::FIRST)) {
		ptr->init(job_organazer, ecs::registry);
	}
}

gs::gs_loop_service::~gs_loop_service()
{
	for (auto& ptr : ecs::job_base::get_jobs(ecs::job_base::FIRST)) {
		ptr->deinit(job_organazer, ecs::registry);
	}
}

void gs::gs_loop_service::init(ds::app_data_storage& storage)
{
	auto& window_system_ref = storage.require<wnd::window_system>();

	window_system_ref.init_windows_frame_time();
	previous_time = std::chrono::high_resolution_clock::now();

	ecs::registry.ctx().emplace<scn::delta_time>(delta_time);
}

void gs::gs_loop_service::on_step(ds::app_data_storage& storage)
{
	auto job_graph = job_organazer.graph();

	auto& window_system_ref = storage.require<wnd::window_system>();
	auto current_time = std::chrono::high_resolution_clock::now();
	std::chrono::duration<float> duration = current_time - previous_time;
	delta_time = duration.count();
	ecs::registry.ctx().get<scn::delta_time>().dt = delta_time;
	previous_time = current_time;

	window_system_ref.pool_events();

	inp::get_system().process_input(delta_time);

	// window->update(context);

	for (auto&& system : job_graph) {
		system.prepare(ecs::registry);
		system.callback()(system.data(), ecs::registry);
	}

	gs::get_system().end_ecs_frame();

	rnd::get_system().render();

	// window->render(context);

	window_system_ref.process_windows();
	stop_requested = window_system_ref.is_stop_running();
}

bool gs::gs_loop_service::should_stop() const
{
	return stop_requested;
}