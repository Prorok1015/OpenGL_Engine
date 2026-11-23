#pragma once
#include <cstdint>
#include <snk/render/rnd_render_context.h>
#include <snk/driver/rnd_driver_interface.h>

namespace rnd {
	// A module fills this vtable (render_system::add_render_pass) and the engine calls it (#1298): raised with any change of
	// its bases or virtual functions (check_vtable_pins.py).
	inline constexpr std::uint32_t render_pass_interface_abi_revision = 1;

	struct render_pass_interface {
		virtual ~render_pass_interface() = default;
		virtual void execute(render_context& context, driver::driver_interface& driver) = 0;

		// A pass that works on the whole frame (it uploads the data of all packets) runs once, over every packet,
		// before the packets are drawn group by group. Every other pass runs once per group of equal render
		// priority and sees only the packets of that group (render_system::render_frame).
		virtual bool once_per_frame() const { return false; }
	};
}
