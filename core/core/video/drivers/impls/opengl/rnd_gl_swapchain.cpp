#include <snk/driver_gl/rnd_gl_swapchain.h>
#include <snk/common/common.h>
#include <glad/glad.h>
#include "rnd_gl_driver.h"
#include <snk/common/logger/engine_log.h>

namespace {
	// GLADloadproc is a bare function pointer, so the surface travels through a global during the load.
	wnd::gl_surface* s_loading_surface = nullptr;

	void* load_gl_proc(const char* name)
	{
		return s_loading_surface->get_proc_address(name);
	}
}

void APIENTRY DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) {
	if (type == GL_DEBUG_TYPE_OTHER) {
		return;
	}

	egLOG("opengl/debug", "OpenGL Debug Message: \"{0}\"", message);
}

rnd::driver::gl::swapchain::swapchain(wnd::gl_surface& surface)
	: m_surface(surface)
{
	m_surface.make_current();
	s_loading_surface = &m_surface;
	const bool success = gladLoadGLLoader(load_gl_proc);
	s_loading_surface = nullptr;
	ASSERT_MSG(success, "glad didnt loaded in this process!");
	GLint maxTextureSize;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

	GLint maxVertexUniformVectors;
	glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &maxVertexUniformVectors);
	GLint maxFragmentUniformVectors;
	glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &maxFragmentUniformVectors);
	GLint maxUniformBlockSize;
	glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxUniformBlockSize);
	GLint maxUniformBufferBindings;
	glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &maxUniformBufferBindings);
	GLint maxTextureUnits;
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
	GLint maxVertexTextureUnits;
	glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &maxVertexTextureUnits);
	GLint maxShaderStorageBlockSize;
	glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxShaderStorageBlockSize);

	egLOG("render_context/init",
		"OpenGL Info: \n"
		"Vendor:   {0}  \n"
		"Renderer: {1}\n"
		"Version:      {2}\n"
		"GLSL Version: {3}\n"
		"Max Texture2D size:                 {4}x{4}\n"
		"Max Vertex Uniform Vectors(vec4):   {5}\n"
		"Max Fragment Uniform Vectors(vec4): {6}\n"
		"Max Uniform Block Size(b):          {7}\n"
		"Max Uniform Buffer Bindings:        {8}\n"
		"Max image units in fragment: {9}, vertex: {10}\n"
		"Max shader storage block size: {11} bytes\n"
		,
		(const char*)glGetString(GL_VENDOR),
		(const char*)glGetString(GL_RENDERER),
		(const char*)glGetString(GL_VERSION),
		(const char*)glGetString(GL_SHADING_LANGUAGE_VERSION),
		maxTextureSize,
		maxVertexUniformVectors,
		maxFragmentUniformVectors,
		maxUniformBlockSize,
		maxUniformBufferBindings,
		maxTextureUnits,
		maxVertexTextureUnits,
		maxShaderStorageBlockSize
		);

	ASSERT_MSG(GLVersion.major > 4 || (GLVersion.major == 4 && GLVersion.minor >= 5), "Engine requires at least OpenGL version 4.5!");

	glEnable(GL_DEBUG_OUTPUT);
	glDebugMessageCallback(DebugCallback, nullptr);
}

std::unique_ptr<rnd::driver::driver_interface> rnd::driver::gl::swapchain::create_driver()
{
	return std::make_unique<gl::driver>();
}

void rnd::driver::gl::swapchain::present()
{
	m_surface.swap_buffers();
}

void rnd::driver::gl::swapchain::set_vsync(bool enabled)
{
	m_surface.make_current();
	m_surface.set_swap_interval(enabled ? 1 : 0);
}
