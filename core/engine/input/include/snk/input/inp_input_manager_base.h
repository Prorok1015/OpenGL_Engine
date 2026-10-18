#pragma once
#include <string>
#include <snk/core_input/inp_input_event.hpp>
#include <snk/window/wnd_window_event_listener_interface.h>

namespace inp
{
	class input_manager_base
	{
	public:
		input_manager_base(const std::string_view name_)
			: name(name_)
		{
		}
		virtual ~input_manager_base() {}
		virtual bool on_handle_event(wnd::handle win, const inp::input_event& evt) = 0;
	private:
		std::string name = "unknown";
	};
}