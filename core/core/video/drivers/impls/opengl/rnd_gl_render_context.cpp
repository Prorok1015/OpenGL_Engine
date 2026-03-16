#include "rnd_gl_render_context.h"
#include <common.h>
#include "rnd_gl_driver.h"
#include <engine_log.h>

int gMaxTexture2DSize = 0;

void APIENTRY DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) {
	if (type == GL_DEBUG_TYPE_OTHER) {
		return;
	}

	egLOG("opengl/debug", "OpenGL Debug Message: \"{0}\"", message);
}

rnd::driver::gl::render_context::render_context(GLADloadproc load)
	: loader(load) 
{
	const bool success = gladLoadGLLoader(load);
	ASSERT_MSG(success, "glad didnt loaded in this process!");
	GLint maxTextureSize;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
	gMaxTexture2DSize = maxTextureSize;

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

std::unique_ptr<rnd::driver::driver_interface> rnd::driver::gl::render_context::create_driver()
{
	return std::make_unique<gl::driver>();
}
