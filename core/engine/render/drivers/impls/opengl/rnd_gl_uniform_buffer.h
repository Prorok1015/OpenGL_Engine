#pragma once
#include <vector>
#include <glad/glad.h> 
#include <rnd_uniform_buffer_interface.h>


namespace rnd::driver::gl
{
	class uniform_buffer : public uniform_buffer_interface
	{
	public:
		uniform_buffer(std::size_t size, std::size_t binding);
		virtual ~uniform_buffer() override;

		virtual void set_data(const void* data, std::size_t size, std::size_t offset = 0) override;
	private:
		GLuint m_RendererID = 0;
	};
}