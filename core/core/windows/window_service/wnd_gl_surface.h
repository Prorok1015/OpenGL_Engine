#pragma once
#include "snk_api.h"

namespace wnd
{
	// What a GL driver needs from a window: its context, buffer swap and symbol lookup.
	// No backend types here, so the driver never links the windowing library.
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
