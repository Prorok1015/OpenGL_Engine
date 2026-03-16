#pragma once
#include "rnd_ssbo_buffer_interface.h"
#include <glad/glad.h>

namespace rnd::driver::gl
{
	class gl_ssbo_buffer : public ssbo_buffer_interface
	{
	public:
		gl_ssbo_buffer(std::size_t size, std::size_t binding);
		virtual ~gl_ssbo_buffer() override;
		virtual void set_data(const void* data, std::size_t size, std::size_t offset = 0) override;
		virtual void bind(std::size_t index) override;
	private:
		GLuint renderer_id = 0;
		std::size_t buffer_size = 0;
	};
}