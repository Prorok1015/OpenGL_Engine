#include "rnd_gl_shader.h"
#include "open_gl_specific.h"
#include <glm/gtc/type_ptr.hpp>

rnd::driver::gl::shader::~shader()
{
	glDeleteProgram(id);
}

void rnd::driver::gl::shader::uniform(std::string_view name, const uniform_data& val)
{
	std::visit([&](auto&& value) { uniform(name, value); }, val);
}

void rnd::driver::gl::shader::uniform(std::string_view name, glm::mat4 val)
{
	GLuint transformLoc = glGetUniformLocation(id, name.data());
	CHECK_GL_ERROR();
	glUniformMatrix4fv(transformLoc, 1, GL_FALSE, glm::value_ptr(val));
	CHECK_GL_ERROR();
}

void rnd::driver::gl::shader::uniform(std::string_view name, glm::ivec4 val)
{
	GLuint transformLoc = glGetUniformLocation(id, name.data());
	CHECK_GL_ERROR();
	glUniform4iv(transformLoc, 1, glm::value_ptr(val));
	CHECK_GL_ERROR();
}

void rnd::driver::gl::shader::uniform(std::string_view name, glm::vec4 val)
{
	GLuint transformLoc = glGetUniformLocation(id, name.data());
	CHECK_GL_ERROR();
	glUniform4fv(transformLoc, 1, glm::value_ptr(val));
	CHECK_GL_ERROR();
}


void rnd::driver::gl::shader::uniform(std::string_view name, glm::vec3 val)
{
	GLuint transformLoc = glGetUniformLocation(id, name.data());
	CHECK_GL_ERROR();
	glUniform3fv(transformLoc, 1, glm::value_ptr(val));
	CHECK_GL_ERROR();
}

void rnd::driver::gl::shader::uniform(std::string_view name, glm::vec2 val)
{
	GLuint transformLoc = glGetUniformLocation(id, name.data());
	CHECK_GL_ERROR();
	glUniform2fv(transformLoc, 1, glm::value_ptr(val));
	CHECK_GL_ERROR();
}

void rnd::driver::gl::shader::uniform(std::string_view name, int val)
{
	glUniform1i(glGetUniformLocation(id, name.data()), val);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::shader::uniform(std::string_view name, float val)
{
	glUniform1f(glGetUniformLocation(id, name.data()), val);
	CHECK_GL_ERROR();
}

void rnd::driver::gl::shader::use()
{
	glUseProgram(id);
	CHECK_GL_ERROR();
}
