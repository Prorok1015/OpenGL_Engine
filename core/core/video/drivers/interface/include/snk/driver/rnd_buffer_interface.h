#pragma once
#include <cstdint>
#include <snk/driver/rnd_buffer_layout.h>

namespace rnd::driver
{
	enum class BUFFER_BINDING
	{
		STATIC, DYNAMIC
	};

	// Module code calls this vtable through render_system::get_driver() (#1298): raised with any change of its bases or
	// virtual functions (check_vtable_pins.py).
	inline constexpr std::uint32_t buffer_interface_abi_revision = 1;

	class buffer_interface
	{
	public:
		virtual ~buffer_interface() {}
		// size == count * stride
		virtual void set_data(const void* data, size_t size, BUFFER_BINDING binding) = 0;

		template<class T>
		void set_data(const std::vector<T>& data, BUFFER_BINDING binding = BUFFER_BINDING::DYNAMIC) {
			set_data(data.data(), data.size() * sizeof(T), binding);
		}

		template<class T>
		void set_data_ptr(const T* data, std::size_t count, BUFFER_BINDING binding = BUFFER_BINDING::DYNAMIC) {
			set_data(data, count * sizeof(T), binding);
		}

		void reserve(std::size_t size) {
			set_data(nullptr, size, rnd::driver::BUFFER_BINDING::DYNAMIC);
		}

		virtual const BufferLayout& get_layout() const = 0;
		virtual void set_layout(const BufferLayout& layout_) = 0;
	};
}