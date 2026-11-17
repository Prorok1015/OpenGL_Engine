#pragma once
#include <snk/common/snk_api.h>

#include <cstdint>

namespace rnd::driver
{
	// The next never-used shader serial. Defined once in snk::common: a counter inside an inline function would
	// be one per module in a shared build, and two shaders could end up with the same serial.
	SNK_API std::uint64_t next_shader_serial() noexcept;

	// The next driver id for the generations of texture handles: 1..255 in turn, never 0. Defined once in
	// snk::common for the same reason as the shader serial: two drivers must not draw from separate counters.
	SNK_API std::uint32_t next_texture_driver_id() noexcept;
}
