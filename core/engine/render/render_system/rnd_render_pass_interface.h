#pragma once
#include "rnd_frame_context.h"
#include "rnd_driver_interface.h"

namespace rnd {
	struct render_pass_interface {
		virtual ~render_pass_interface() = default;
		virtual void execute(frame_context& context, driver::driver_interface& driver) = 0;
	};
}
