#pragma once
#include <chrono>
#include <optional>

namespace edt
{
	// When to say that a level load is still running (#613): once per period. A slow load is no hang, so nothing
	// cancels it by itself; the notice points at level.cancel_load. Times come from the caller (tests pass their own).
	class slow_load_notice
	{
	public:
		using clock = std::chrono::steady_clock;

		explicit slow_load_notice(clock::duration period = std::chrono::seconds(30)) : m_period(period) {}

		void start(clock::time_point now)
		{
			m_started = now;
			m_next = now + m_period;
		}

		// How long the load has run, when a notice is due at `now`; nullopt otherwise.
		std::optional<std::chrono::seconds> due(clock::time_point now)
		{
			if (now < m_next) return std::nullopt;
			m_next = now + m_period;
			return std::chrono::duration_cast<std::chrono::seconds>(now - m_started);
		}

	private:
		clock::duration m_period;
		clock::time_point m_started{};
		clock::time_point m_next{};
	};
}
