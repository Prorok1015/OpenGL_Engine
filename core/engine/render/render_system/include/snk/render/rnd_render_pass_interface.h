#pragma once
#include <snk/render/rnd_render_context.h>
#include <snk/driver/rnd_driver_interface.h>

namespace rnd {
	struct render_pass_interface {
		virtual ~render_pass_interface() = default;
		virtual void execute(render_context& context, driver::driver_interface& driver) = 0;
	};
}
