#pragma once
#include <snk/application/app_deterministic_time.h>
#include <snk/common/snk_api.h>
#include <cstdint>
#include <optional>

namespace app
{
	// Steps the simulation by a chosen number of frames while the process runs. Until the first advance() it does
	// nothing and the loop keeps the time it computed itself (wall clock, or the fixed dt / freeze of
	// deterministic_time). From the first advance() on, the loop's time is held: every granted frame runs with
	// `fixed_dt`, every other frame with dt 0, so the scene stays as it is while windows, UI and commands go on.
	// Generalizes `app/freeze_after_frames`, which freezes once and cannot be thawed.
	// Main thread only: advance() is a command (`app.advance`), the loop calls the rest.
	class SNK_API frame_control
	{
	public:
		// 1/60 s unless `time` carries a fixed dt.
		explicit frame_control(const deterministic_time& time = {});

		// Grants `frames` more frames at `fixed_dt` (kept for the later grants when given; not above 0: ignored).
		// The first call holds the loop's time.
		void advance(std::uint64_t frames, std::optional<float> fixed_dt = std::nullopt);

		bool holding() const { return m_holding; }
		// Granted frames the loop has not run yet.
		std::uint64_t pending() const { return m_pending; }
		float fixed_dt() const { return m_fixed_dt; }
		// Frames the loop has started, the running one included.
		std::uint64_t frame() const { return m_frame; }
		// The frame count at which the loop found the deterministic scene settled; none before that.
		std::optional<std::uint64_t> settled_frame() const { return m_settled_frame; }

		// Loop side. begin_frame() opens a frame; take_dt() gives the dt the frame runs with, for the dt the loop
		// computed. Not held: that dt. Held: fixed_dt for a granted frame, 0 otherwise.
		void begin_frame() { ++m_frame; }
		float take_dt(float loop_dt);
		void mark_settled() { m_settled_frame = m_frame; }
		// A new run of the loop starts counting again; the time hold stays as the commands left it.
		void restart() { m_frame = 0; m_settled_frame.reset(); }

	private:
		float m_fixed_dt;
		bool m_holding = false;
		std::uint64_t m_pending = 0;
		std::uint64_t m_frame = 0;
		std::optional<std::uint64_t> m_settled_frame;
	};
}
