#pragma once
#include <snk/common/common.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/common/snk_api.h>

namespace rnd
{
	SNK_API std::string to_string(rnd::driver::RENDER_MODE mode);
	SNK_API rnd::driver::RENDER_MODE render_mode_from_string(const std::string& str);
}