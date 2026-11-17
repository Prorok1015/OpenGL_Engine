#include "rnd_gl_shader.h"
#include "open_gl_specific.h"

rnd::driver::gl::shader::~shader()
{
	glDeleteProgram(id);
}

void rnd::driver::gl::shader::use() const
{
	glUseProgram(id);
	CHECK_GL_ERROR();
}
