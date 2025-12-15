#pragma once
#include <string>
#include "inp_events.hpp"

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
		virtual bool on_handle_event(wnd::handle win, const wnd::input_event& evt) = 0;
	private:
		std::string name = "unknown";
	};
}