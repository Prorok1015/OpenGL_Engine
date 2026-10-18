#include "app_loop_service.h"
#include "eng_profiler.h"

app::loop_service::loop_service(frame_stage_registry& stages)
	: m_stages(stages)
{
}

void app::loop_service::init(ds::app_data_storage& storage)
{
	m_stop_requested = false;
	m_stages.run_init_hooks(storage);
	m_previous_time = std::chrono::steady_clock::now();
}

void app::loop_service::on_step(ds::app_data_storage& storage)
{
	PROFILE_FRAME("MainThread");
	PROFILE_SCOPE("FrameTotal");

	const auto current_time = std::chrono::steady_clock::now();
	const std::chrono::duration<float> dt = current_time - m_previous_time;
	m_previous_time = current_time;

	stage_context ctx{ storage, dt };
	m_stages.run(ctx);
}

void app::loop_service::term(ds::app_data_storage& storage)
{
	m_stages.run_term_hooks(storage);
}
