#pragma once
#include <vector>
#include <glad/glad.h>
#include <rnd_buffer_interface.h>
#include <rnd_buffer_layout.h>

namespace rnd::driver::gl
{
	class buffer : public rnd::driver::buffer_interface
	{
	public:
		buffer();
		virtual ~buffer() override;
		virtual void set_data(const void* data, std::size_t size, BUFFER_BINDING binding) override;
		virtual const BufferLayout& get_layout() const override { return layout; }
		virtual void set_layout(const BufferLayout& layout_) override { layout = layout_; }

		GLuint get_id() const { return buffer_id; }

	private:
		bool isAllocatedMemory = false;
		std::size_t allocated_size = 0;
		GLuint buffer_id = 0;
		GLint buffer_type = 0;
		BufferLayout layout;
	};
}