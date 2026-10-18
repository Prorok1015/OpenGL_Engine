#pragma once
#include <snk/driver/rnd_swapchain_interface.h>
#include <snk/common/snk_api.h>

#include <cstdint>

namespace rnd::driver::null
{
	// Presentation without a surface: present only counts frames.
	class SNK_API swapchain final : public rnd::driver::swapchain_interface
	{
	public:
		std::unique_ptr<rnd::driver::driver_interface> create_driver() override;
		void present() override { ++m_frames_presented; }
		void set_vsync(bool enabled) override { m_vsync = enabled; }

		std::uint64_t frames_presented() const { return m_frames_presented; }
		bool vsync() const { return m_vsync; }

	private:
		std::uint64_t m_frames_presented = 0;
		bool m_vsync = false;
	};
}
