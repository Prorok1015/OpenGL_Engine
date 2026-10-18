#pragma once
#include "common.h"
#include "rnd_driver_interface.h"
#include "snk_api.h"

namespace rnd
{
	SNK_API std::string to_string(rnd::driver::RENDER_MODE mode);
	SNK_API rnd::driver::RENDER_MODE render_mode_from_string(const std::string& str);
}