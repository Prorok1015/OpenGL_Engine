#pragma once 
#include <memory>
#include "rnd_buffer_interface.h"

namespace rnd::driver
{
	class vertex_array_interface
	{
	public:
		virtual ~vertex_array_interface() {}
		virtual void bind() const = 0;
		virtual void unbind() const = 0;

		virtual void add_vertex_buffer(const std::shared_ptr<buffer_interface>& vertexBuffer) = 0;
		virtual void remove_vertex_buffer(const std::shared_ptr<buffer_interface>& vertexBuffer) = 0;
		virtual void set_index_buffer(const std::shared_ptr<buffer_interface>& indexBuffer) = 0;
	};
}