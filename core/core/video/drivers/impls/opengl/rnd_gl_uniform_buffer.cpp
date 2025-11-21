#include "rnd_gl_uniform_buffer.h"
#include "open_gl_specific.h"

rnd::driver::gl::uniform_buffer::uniform_buffer(std::size_t size, std::size_t binding)
{
	glCreateBuffers(1, &m_RendererID);
	CHECK_GL_ERROR();

	glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
	CHECK_GL_ERROR();

	glBindBufferRange(GL_UNIFORM_BUFFER, binding, m_RendererID, 0, size);
	CHECK_GL_ERROR();
}

rnd::driver::gl::uniform_buffer::~uniform_buffer()
{
	glDeleteBuffers(1, &m_RendererID);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::uniform_buffer::set_data(const void* data, std::size_t size, std::size_t offset)
{
	glNamedBufferSubData(m_RendererID, offset, size, data);
	CHECK_GL_ERROR();
}
