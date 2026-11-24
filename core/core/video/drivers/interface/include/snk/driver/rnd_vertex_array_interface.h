#pragma once 
#include <cstdint>
#include <memory>
#include <snk/driver/rnd_buffer_interface.h>

namespace rnd::driver
{
	// Module code calls this vtable through render_system::get_driver() (#1298): raised with any change of its bases or
	// virtual functions (check_vtable_pins.py).
	inline constexpr std::uint32_t vertex_array_interface_abi_revision = 1;

	class vertex_array_interface
	{
	public:
		virtual ~vertex_array_interface() {}
		virtual void bind() const = 0;
		virtual void unbind() const = 0;

		virtual void add_vertex_buffer(const std::shared_ptr<buffer_interface>& vertexBuffer) = 0;
		virtual void set_index_buffer(const std::shared_ptr<buffer_interface>& indexBuffer) = 0;
	};
}