#include "rnd_gl_texture.h"
#include "open_gl_specific.h"

rnd::driver::gl::texture::texture(GLuint texture_id, int w, int h)
	: id(texture_id)
	, width_i(w)
	, height_i(h)
{
}

void rnd::driver::gl::texture::bind()
{
	glBindTexture(GL_TEXTURE_2D, id);
	CHECK_GL_ERROR();
}

int rnd::driver::gl::texture::width() const
{
	return width_i;
}

int rnd::driver::gl::texture::height() const
{
	return height_i;
}