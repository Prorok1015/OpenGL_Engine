#include "application.h"
#include <wnd_window_system.h>
#include <rnd_render_system.h>
#include <inp_input_system.h>
#include <ecs_system.h>
#include "ecs_common_system.h"
#include <gs_game_system.h>
#include <Windows.h>
#include <chrono>
#include <entt/entt.hpp>

app::application* p_app_system = nullptr;

app::application& app::get_app_system()
{
	ASSERT_MSG(p_app_system, "application system is nullptr!");
	return *p_app_system;
}

app::application::application()
{
	for (auto& ptr : ecs::job_base::get_jobs(ecs::job_base::FIRST)) {
		ptr->init(job_organazer, ecs::registry);
	}
}

app::application::~application()
{
	for (auto& ptr : ecs::job_base::get_jobs(ecs::job_base::FIRST)) {
		ptr->deinit(job_organazer, ecs::registry);
	}
}

int app::application::run()
{
	auto& window_system_ref = wnd::get_system();

	window_system_ref.init_windows_frame_time();

	float delta_time = 1.0f / 60.0f;
	auto previous_time = std::chrono::high_resolution_clock::now();

	auto job_graph = job_organazer.graph();

	ecs::registry.ctx().emplace<scn::delta_time>(delta_time);

	while (!window_system_ref.is_stop_running()) {
		auto current_time = std::chrono::high_resolution_clock::now();
		std::chrono::duration<float> duration = current_time - previous_time;
		delta_time = duration.count();
		ecs::registry.ctx().get<scn::delta_time>().dt = delta_time;
		previous_time = current_time;

		wnd::get_system().pool_events();

		inp::get_system().process_input(delta_time);
		 
		for (auto&& system : job_graph) {
			system.prepare(ecs::registry);
			system.callback()(system.data(), ecs::registry);
		}

		gs::get_system().end_ecs_frame();

		rnd::get_system().render();
 
		window_system_ref.process_windows();
	}

	return 0;
}