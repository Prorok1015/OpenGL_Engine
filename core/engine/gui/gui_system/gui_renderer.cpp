#include "gui_renderer.h"

gui::renderer::renderer(gui::imgui_backend_interface* backend_)
	: rnd::renderer_base(0)
	, backend(backend_)
{
}

void gui::renderer::on_render(rnd::driver::driver_interface* drv)
{
	backend->new_frame();
	render_event();
	backend->render();
}