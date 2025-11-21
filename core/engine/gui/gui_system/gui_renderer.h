#pragma once
#include <common.h>
#include <rnd_renderer_base.h>
#include <rnd_driver_interface.h>
#include "gui_backend_interface.h"

namespace gui
{
	class renderer : public rnd::renderer_base
	{
	public:
		renderer(gui::imgui_backend_interface* backend);
		virtual ~renderer() override {}

		virtual void on_render(rnd::driver::driver_interface* drv);

	public:
		Event<> render_event;

	private:
		gui::imgui_backend_interface* backend = nullptr;
	};
}