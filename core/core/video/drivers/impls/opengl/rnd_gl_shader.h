#pragma once
#include <snk/driver/rnd_shader_interface.h>
#include <glad/glad.h>

namespace rnd::driver::gl
{
	class shader : public rnd::driver::shader_interface
	{
	public:
		shader(GLuint shader_id) : id(shader_id) {}
		virtual ~shader() override;

		// Makes the program current; the driver does it when a pipeline is bound.
		void use() const;
		GLuint program() const { return id; }

	private:
		GLuint id = 0;
	};
}