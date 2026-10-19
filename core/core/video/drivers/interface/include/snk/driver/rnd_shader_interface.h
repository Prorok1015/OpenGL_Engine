#pragma once
#include <snk/common/shader_serial.h>
#include <cstdint>

namespace rnd::driver
{
	class shader_interface
	{
	public:
		virtual ~shader_interface() {}

		// Names this shader for as long as the process runs. Unlike its address it is never handed out again,
		// so a pipeline cannot take a new shader for an old one that lived at the same place.
		std::uint64_t serial() const noexcept { return m_serial; }

	private:
		const std::uint64_t m_serial = next_shader_serial();
	};
}