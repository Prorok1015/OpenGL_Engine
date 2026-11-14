#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// What the editor does about migrations a project was left with (EPIC-58 S8, #761) once the project is open and its
	// start level settled: a person is asked whether to roll the first one back now; headless nobody is asked, a WARN names
	// the command line that does it. No ImGui.
	struct journal_offer
	{
		enum class kind : std::uint8_t { none, ask, warn };

		kind what = kind::none;
		// The migration to roll back: the first unfinished one (they are sorted by id, so the oldest).
		std::string id;
		std::string title;
		std::string text;
	};

	inline constexpr std::string_view k_journal_offer_roll_back = "Roll Back";
	inline constexpr std::string_view k_journal_offer_later = "Later";

	// `unfinished`: prj::migration::unfinished_migrations of the project's root.
	journal_offer decide_journal_offer(const std::vector<std::string>& unfinished, bool headless);
}
