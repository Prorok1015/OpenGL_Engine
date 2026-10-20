#pragma once
#include <snk/driver/rnd_swapchain_interface.h>
#include <snk/window/wnd_gl_surface.h>

namespace rnd::driver::gl
{
	// Makes the surface's context current, loads glad through it and owns swap + vsync.
	// The surface must outlive the swapchain.
	class swapchain final : public rnd::driver::swapchain_interface
	{
	public:
		explicit swapchain(wnd::gl_surface& surface);
		~swapchain() override {}

		std::unique_ptr<rnd::driver::driver_interface> create_driver() override;
		void present() override;
		void set_vsync(bool enabled) override;

	private:
		wnd::gl_surface& m_surface;
	};
}
