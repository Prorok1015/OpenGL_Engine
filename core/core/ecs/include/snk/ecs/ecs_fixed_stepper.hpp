#pragma once
#include <cmath>

namespace ecs {

// Fixed-step time accumulator of a context. The same sequence of frame deltas always yields the
// same sequence of step counts (double accumulation, no clock reads), which is what makes
// fixed_update deterministic.
//  - a frame delta is clamped to [0, max_frame_dt] (NaN counts as 0), so a long stall (debugger,
//    window drag) does not turn into a burst of simulation;
//  - at most max_steps steps run per frame; the backlog beyond that is dropped, keeping only
//    the fraction of a step, so a slow frame cannot snowball.
class fixed_stepper
{
public:
	explicit fixed_stepper(double step = 1.0 / 60.0, double max_frame_dt = 0.25, int max_steps = 8) noexcept
		: m_step(step), m_max_frame_dt(max_frame_dt), m_max_steps(max_steps) {}

	[[nodiscard]] double clamp_dt(double dt) const noexcept
	{
		if (!(dt > 0.0)) return 0.0;
		return dt < m_max_frame_dt ? dt : m_max_frame_dt;
	}

	// Adds a frame delta; returns how many fixed steps to run this frame.
	int advance(double dt) noexcept
	{
		m_accumulator += clamp_dt(dt);
		int steps = 0;
		while (m_accumulator >= m_step && steps < m_max_steps) {
			m_accumulator -= m_step;
			++steps;
		}
		if (m_accumulator >= m_step) m_accumulator = std::fmod(m_accumulator, m_step);
		return steps;
	}

	void reset() noexcept { m_accumulator = 0.0; }

	[[nodiscard]] float step() const noexcept { return static_cast<float>(m_step); }
	// Position inside the current step in [0, 1), for interpolating rendering.
	[[nodiscard]] float alpha() const noexcept { return static_cast<float>(m_accumulator / m_step); }
	[[nodiscard]] int max_steps() const noexcept { return m_max_steps; }

private:
	double m_step;
	double m_max_frame_dt;
	int m_max_steps;
	double m_accumulator = 0.0;
};

} // namespace ecs
