#include "rnd_gl_buffer.h"
#include "open_gl_specific.h"
#include <engine_assert.h>

const GLint gBufferTypeToGlBufferType[] =
{
	GL_ARRAY_BUFFER,
	GL_ELEMENT_ARRAY_BUFFER,
};

rnd::driver::gl::buffer::buffer()
{
	glCreateBuffers(1, &buffer_id);
	CHECK_GL_ERROR();
}

rnd::driver::gl::buffer::~buffer()
{
	glDeleteBuffers(1, &buffer_id);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::buffer::set_data(const void* data, std::size_t size, BUFFER_BINDING binding)
{
	if (data && isAllocatedMemory) {
		ASSERT_MSG(size < allocated_size, "You trying set more info then was allocated!");
		glNamedBufferSubData(buffer_id, 0, size, data);
		CHECK_GL_ERROR();
	} else {
		isAllocatedMemory = true;
		allocated_size = size;
		glNamedBufferData(buffer_id, size, data, (binding == BUFFER_BINDING::DYNAMIC ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW));
		CHECK_GL_ERROR();
	}
}
