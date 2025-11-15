#pragma once
#include <string>
#include "inp_events.hpp"

namespace inp
{
	class input_manager_base
	{
	public:
		input_manager_base(const std::string_view name_, int priority_)
			: name(name_)
			, priority(priority_)
		{
		}
		virtual ~input_manager_base() {}

		virtual void on_notify_listeners(float dt) = 0;
		virtual bool on_handle_event(const input_event& evt) = 0;

		int get_priority() const { return priority; }

	private:
		std::string name = "unknown";
		int priority = 0;
	};
}