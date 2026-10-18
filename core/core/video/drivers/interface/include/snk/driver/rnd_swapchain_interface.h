#pragma once
#include <memory>
#include <snk/driver/rnd_driver_interface.h>

namespace rnd::driver
{
	// Owns the presentation side of a rendering API: creates the driver and presents frames.
	class swapchain_interface
	{
	public:
		virtual ~swapchain_interface() {}

		virtual std::unique_ptr<driver_interface> create_driver() = 0;
		virtual void present() = 0;
		virtual void set_vsync(bool enabled) = 0;
	};
}
