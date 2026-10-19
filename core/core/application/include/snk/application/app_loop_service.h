#pragma once
#include <chrono>
#include <cstdint>
#include <snk/application/app_deterministic_time.h>
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service_interface.h>
#include <snk/common/snk_api.h>

namespace app
{
	// The one frame loop of every host (editor, game): times the frame and runs the stages of the
	// registry it is given. Host-specific behaviour is added by modules as stages; the loop itself
	// knows neither the window nor the renderer.
	class SNK_API loop_service : public app_loop_service_interface
	{
	public:
		explicit loop_service(frame_stage_registry& stages, deterministic_time time = {});

		// Runs the registry's init hooks and starts the frame clock.
		void init(ds::app_data_storage& storage) override;
		void on_step(ds::app_data_storage& storage) override;
		// Runs the registry's term hooks (reverse order).
		void term(ds::app_data_storage& storage) override;
		bool should_stop() const override { return m_stop_requested; }
		void request_stop() override { m_stop_requested = true; }

	private:
		frame_stage_registry& m_stages;
		deterministic_time m_time;
		std::uint64_t m_frames = 0;
		bool m_stop_requested = false;
		std::chrono::steady_clock::time_point m_previous_time;
	};
}
