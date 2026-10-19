#include <snk/application/app_frame_control.h>

namespace {
	constexpr float default_fixed_dt = 1.f / 60.f;
}

app::frame_control::frame_control(const deterministic_time& time)
	: m_fixed_dt(time.fixed_dt > 0.f ? time.fixed_dt : default_fixed_dt)
{
}

void app::frame_control::advance(std::uint64_t frames, std::optional<float> fixed_dt)
{
	if (fixed_dt && *fixed_dt > 0.f) {
		m_fixed_dt = *fixed_dt;
	}
	m_holding = true;
	m_pending += frames;
}

void app::frame_control::release()
{
	m_holding = false;
	m_pending = 0;
}

float app::frame_control::take_dt(float loop_dt)
{
	if (!m_holding) {
		return loop_dt;
	}
	if (m_pending == 0) {
		return 0.f;
	}
	--m_pending;
	return m_fixed_dt;
}
