#include "rnd_gl_ssbo_buffer.h"
#include "open_gl_specific.h"

rnd::driver::gl::gl_ssbo_buffer::gl_ssbo_buffer(std::size_t size, std::size_t binding)
	: buffer_size(size)
{
	glCreateBuffers(1, &renderer_id);
	glNamedBufferData(renderer_id, size, nullptr, GL_DYNAMIC_DRAW);
	bind(binding);
}

rnd::driver::gl::gl_ssbo_buffer::~gl_ssbo_buffer()
{
	glDeleteBuffers(1, &renderer_id);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::gl_ssbo_buffer::set_data(const void* data, std::size_t size, std::size_t offset)
{
	glNamedBufferSubData(renderer_id, offset, size, data);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::gl_ssbo_buffer::bind(std::size_t index)
{
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer_id);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(index), renderer_id);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	CHECK_GL_ERROR();
}
