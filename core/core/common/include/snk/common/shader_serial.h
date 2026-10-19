#pragma once
#include <snk/common/snk_api.h>

#include <cstdint>

namespace rnd::driver
{
	// The next never-used shader serial. Defined once in snk::common: a counter inside an inline function would
	// be one per module in a shared build, and two shaders could end up with the same serial.
	SNK_API std::uint64_t next_shader_serial() noexcept;
}
