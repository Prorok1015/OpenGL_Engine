#pragma once
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace edt
{
	// What a save of a file older than its type says (#761, R26 of EPIC-58): the file is written in the current version,
	// which older editors do not open. The same words in the log and in the menu bar.
	inline std::string version_upgrade_text(std::string_view file, int from, int to)
	{
		return std::format("'{}' was updated from v{} to v{}: older versions of the editor will not open it", file, from, to);
	}

	// What a save says when it writes a mesh it found in JSON arrays as the desc and its binary file (EPIC-79, #1068).
	inline std::string geometry_conversion_text(std::string_view file, std::string_view binary)
	{
		return std::format("'{}' was converted to the binary format (EPIC-79): its vertices and indices are in '{}' now; "
			"older versions of the editor will not open it", file, binary);
	}

	// A line shown for a few seconds (the menu bar's save notice). Times come from the caller (tests pass their own).
	class timed_notice
	{
	public:
		using clock = std::chrono::steady_clock;

		explicit timed_notice(clock::duration shown_for = std::chrono::seconds(5)) : m_shown_for(shown_for) {}

		void show(std::string text, clock::time_point now)
		{
			m_text = std::move(text);
			m_until = now + m_shown_for;
		}

		// The text while it is shown at `now`; nullopt before anything was shown and once its time is over.
		std::optional<std::string_view> text(clock::time_point now) const
		{
			if (m_text.empty() || now >= m_until) return std::nullopt;
			return std::string_view{ m_text };
		}

	private:
		clock::duration m_shown_for;
		std::string m_text;
		clock::time_point m_until{};
	};
}
