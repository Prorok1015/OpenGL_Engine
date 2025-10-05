#pragma once
#include "common.h"
#include "rnd_driver_interface.h"

namespace rnd
{
	std::string to_string(rnd::driver::RENDER_MODE mode);
	rnd::driver::RENDER_MODE render_mode_from_string(const std::string& str);
}