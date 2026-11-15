#pragma once
#include "edt_agent_confirmations.h"
#include <chrono>
#include <cstddef>
#include <vector>

namespace test_util
{
	// One frame of the question window as a test draws it: every waiting request, one below the other.
	inline std::vector<edt::shown_request> confirmation_frame(const edt::agent_confirmations& confirmations)
	{
		std::vector<edt::shown_request> frame;
		std::size_t row = 0;
		for (const edt::confirmation_request& request : confirmations.pending()) {
			const float top = 100.f * static_cast<float>(row++);
			frame.push_back(edt::shown_request{ request.id, edt::shown_rect{ 10.f, top, 110.f, top + 20.f } });
		}
		return frame;
	}

	inline void show_confirmations(edt::agent_confirmations& confirmations)
	{
		confirmations.note_shown(confirmation_frame(confirmations));
	}

	// What a person does before an "allow": the window is on screen, unchanged, for the guard's time.
	inline void show_confirmations_and_wait(edt::agent_confirmations& confirmations, std::chrono::steady_clock::time_point& clock)
	{
		show_confirmations(confirmations);
		show_confirmations(confirmations);
		clock += edt::agent_confirmations::input_guard;
		show_confirmations(confirmations);
	}
}
