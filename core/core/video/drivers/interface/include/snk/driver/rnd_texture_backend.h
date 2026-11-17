#pragma once
#include <snk/driver/rnd_handle.h>
#include <snk/driver/rnd_texture_interface.h>

namespace rnd::driver
{
	// How the code that is part of a backend gets the object behind a texture_handle: a decorator that wraps another
	// driver (validation) and the tests of the drivers. The gl, null and validation drivers implement it next to
	// driver_interface; the interface itself offers no way from a handle to the object, so the renderer, the editor and
	// the GUI work with handles only (what they need of the texture goes through driver_interface: info, read_texture,
	// update_texture, native_texture_name).
	class texture_object_access
	{
	public:
		virtual ~texture_object_access() = default;

		// The backend object of a texture, null for a handle that names no live texture of the driver.
		virtual texture_interface* texture_object(texture_handle texture) const = 0;
	};
}
