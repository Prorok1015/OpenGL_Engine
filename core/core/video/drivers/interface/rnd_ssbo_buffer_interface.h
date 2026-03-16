#pragma once
#include <vector>

namespace rnd::driver
{
	class ssbo_buffer_interface
	{
	public:
		virtual ~ssbo_buffer_interface() {}
		virtual void set_data(const void* data, std::size_t size, std::size_t offset = 0) = 0;
		template<class T>
		void set_data(const std::vector<T>& data, std::size_t offset = 0) {
			set_data(data.data(), data.size() * sizeof(T), offset);
		}

		template<class T>
		void set_data_ptr(const T* data, std::size_t size, std::size_t offset = 0) {
			set_data(data, size * sizeof(T), offset);
		}

		void reserve(std::size_t size) {
			set_data(nullptr, size);
		}

		virtual void bind(std::size_t index) = 0;
	};
}