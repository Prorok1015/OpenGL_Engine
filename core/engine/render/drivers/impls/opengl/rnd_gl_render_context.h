#pragma once
#include <rnd_render_contex_interface.h>
#include <glad/glad.h>

namespace rnd::driver::gl
{
	class render_context : public rnd::driver::render_context_interface
	{
	public:
		render_context(GLADloadproc load);
		virtual ~render_context() override {}

		virtual std::unique_ptr<rnd::driver::driver_interface> create_driver() override;
	private:
		GLADloadproc loader = nullptr;
	};
}
