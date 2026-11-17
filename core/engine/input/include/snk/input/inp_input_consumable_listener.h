#pragma once
#include <snk/input/inp_events.hpp>
#include <snk/window/wnd_window_event_listener_interface.h>

namespace inp
{
	class input_event_consumable_listener_interface
	{
	public:
		virtual ~input_event_consumable_listener_interface() = default;
		virtual bool on_handle_event(wnd::handle win, const inp::input_event& evt) = 0;
	};
}