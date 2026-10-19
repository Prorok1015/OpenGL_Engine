#include "rnd_gl_framebuffers.h"
#include "open_gl_specific.h"

namespace {
	void delete_framebuffer(GLuint& fbo)
	{
		glDeleteFramebuffers(1, &fbo);
		CHECK_GL_ERROR();
	}
}

rnd::driver::gl::framebuffer_registry::~framebuffer_registry()
{
	release_all();
}

void rnd::driver::gl::framebuffer_registry::evict_texture(std::uint64_t object)
{
	cache.evict(object, delete_framebuffer);
}

void rnd::driver::gl::framebuffer_registry::release_all()
{
	cache.clear(delete_framebuffer);
}
