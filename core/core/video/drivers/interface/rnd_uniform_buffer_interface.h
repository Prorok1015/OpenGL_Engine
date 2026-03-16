#pragma once

namespace rnd::driver
{
	//TODO: rename to constant_buffer_interface
	class uniform_buffer_interface
	{
	public:
		virtual ~uniform_buffer_interface() {}
		virtual void set_data(const void* data, std::size_t size, std::size_t offset = 0) = 0;
	};
}