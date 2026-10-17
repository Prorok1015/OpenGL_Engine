#pragma once
#include <memory>
#include "rnd_driver_interface.h"

namespace rnd::driver
{
	class render_context_interface
	{
	public:
		virtual ~render_context_interface() {}

		virtual std::unique_ptr<driver_interface> create_driver() = 0;
	};
}