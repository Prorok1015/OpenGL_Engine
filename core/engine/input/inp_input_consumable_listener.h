#pragma once
#include "inp_events.hpp"

namespace inp
{
	class input_event_consumable_listener_interface
	{
	public:
		virtual ~input_event_consumable_listener_interface() = default;
		virtual bool on_handle_event(wnd::handle win, const wnd::input_event& evt) = 0;
	};
}