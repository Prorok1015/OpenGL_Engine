#pragma once
#include <snk/common/shader_serial.h>
#include <cstdint>

namespace rnd::driver
{
	// Module code calls this vtable through render_system::get_driver() (#1298): raised with any change of its bases or
	// virtual functions (check_vtable_pins.py).
	inline constexpr std::uint32_t shader_interface_abi_revision = 1;

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