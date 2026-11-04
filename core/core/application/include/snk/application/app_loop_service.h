#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <snk/application/app_deterministic_time.h>
#include <snk/application/app_frame_control.h>
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
		// `control` (optional, must outlive the loop) steps the frame time on command (frame_control).
		explicit loop_service(frame_stage_registry& stages, deterministic_time time = {}, frame_control* control = nullptr);

		// Runs the registry's init hooks and starts the frame clock.
		void init(ds::app_data_storage& storage) override;
		void on_step(ds::app_data_storage& storage) override;
		// Runs the registry's term hooks (reverse order).
		void term(ds::app_data_storage& storage) override;
		// Deterministic time only. The freeze counts frames from the first frame `ready` returns true
		// (the scene was instantiated: animation clocks start there). No probe: ready at once.
		void set_ready_probe(std::function<bool(ds::app_data_storage&)> ready) { m_ready_probe = std::move(ready); }
		// Deterministic time only. After the freeze the loop logs "scene settled" on the first frame
		// `settled` returns true (no loads in flight), so a screenshot shows the finished frame.
		void set_settled_probe(std::function<bool(ds::app_data_storage&)> settled) { m_settled_probe = std::move(settled); }
		// > 0: after this many steps the loop requests its own stop (`app/exit_after_frames`); 0: never.
		void set_exit_after_frames(std::uint64_t frames) { m_exit_after_frames = frames; }
		bool should_stop() const override { return m_stop_requested; }
		void request_stop() override { m_stop_requested = true; }

	private:
		frame_stage_registry& m_stages;
		deterministic_time m_time;
		frame_control* m_control;
		std::uint64_t m_frames = 0;
		std::uint64_t m_steps = 0;
		std::uint64_t m_exit_after_frames = 0;
		std::function<bool(ds::app_data_storage&)> m_ready_probe;
		std::function<bool(ds::app_data_storage&)> m_settled_probe;
		bool m_settled = false;
		std::optional<std::uint64_t> m_ready_frame;
		bool m_stop_requested = false;
		std::chrono::steady_clock::time_point m_previous_time;
	};
}
