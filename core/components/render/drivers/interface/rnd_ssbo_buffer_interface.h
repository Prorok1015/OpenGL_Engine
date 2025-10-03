#pragma once
#include <cstddef>

namespace rnd::driver
{
	class ssbo_buffer_interface
	{
	public:
		virtual ~ssbo_buffer_interface() {}
		virtual void set_data(const void* data, std::size_t size, std::size_t offset = 0) = 0;
		virtual void bind(std::size_t index) = 0;
	};
}