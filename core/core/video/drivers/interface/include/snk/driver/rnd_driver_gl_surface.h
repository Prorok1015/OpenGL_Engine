#pragma once
#include <cstdint>
#include <snk/common/snk_api.h>

namespace rnd::driver
{
	// Module code calls this vtable: raised with any change of its bases or virtual functions (check_vtable_pins.py).
	inline constexpr std::uint32_t gl_surface_abi_revision = 1;

	// What the GL driver needs from a window: its context, buffer swap and symbol lookup. The driver declares it and a
	// window implements it, so the driver never depends on the windowing library (#1161).
	class SNK_API gl_surface
	{
	public:
		virtual ~gl_surface() = default;

		virtual void make_current() = 0;
		virtual void swap_buffers() = 0;
		virtual void set_swap_interval(int interval) = 0;
		virtual void* get_proc_address(const char* name) const = 0;
	};
}
