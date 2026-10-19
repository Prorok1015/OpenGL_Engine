#include <snk/application/app_loop_service.h>
#include <snk/common/eng_profiler.h>
#include <snk/common/logger/engine_log.h>

app::loop_service::loop_service(frame_stage_registry& stages, deterministic_time time)
	: m_stages(stages), m_time(time)
{
}

void app::loop_service::init(ds::app_data_storage& storage)
{
	m_stop_requested = false;
	m_frames = 0;
	m_settled = false;
	m_ready_frame.reset();
	m_stages.run_init_hooks(storage);
	m_previous_time = std::chrono::steady_clock::now();
}

void app::loop_service::on_step(ds::app_data_storage& storage)
{
	PROFILE_FRAME("MainThread");
	PROFILE_SCOPE("FrameTotal");

	const auto current_time = std::chrono::steady_clock::now();
	std::chrono::duration<float> dt = current_time - m_previous_time;
	m_previous_time = current_time;

	if (m_time.enabled()) {
		if (!m_ready_frame && (!m_ready_probe || m_ready_probe(storage))) {
			m_ready_frame = m_frames;
		}
		const auto limit = static_cast<std::uint64_t>(m_time.freeze_after_frames);
		const bool frozen = limit > 0 && m_ready_frame && m_frames >= *m_ready_frame + limit;
		if (frozen && m_frames == *m_ready_frame + limit) {
			egLOG_INFO("app", "deterministic time frozen after {} frames (ready at frame {})", limit, *m_ready_frame);
		}
		if (frozen && !m_settled && (!m_settled_probe || m_settled_probe(storage))) {
			m_settled = true;
			// run_headless.sh waits for this line before it takes a screenshot.
			egLOG_INFO("app", "deterministic scene settled at frame {}", m_frames);
		}
		dt = std::chrono::duration<float>(frozen ? 0.f : m_time.fixed_dt);
		++m_frames;
	}

	stage_context ctx{ storage, dt };
	m_stages.run(ctx);
}

void app::loop_service::term(ds::app_data_storage& storage)
{
	m_stages.run_term_hooks(storage);
}
