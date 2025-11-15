#pragma once
#include <rnd_driver_interface.h>

namespace rnd
{
	class renderer_base
	{
	public:
		renderer_base(int priority_)
			: priority(priority_) {}

		virtual ~renderer_base() {}

		virtual void on_render(driver::driver_interface* drv) = 0;

		int get_render_priority() const { return priority; }
	private:
		int priority = 0;
	};
}