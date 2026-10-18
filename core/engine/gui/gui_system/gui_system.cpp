#include "gui_system.h"
#include "engine_assert.h"
#include "rnd_render_system.h"
#include "wnd_window_system.h"
#include "eng_profiler.h"

gui::gui_system::gui_system(gui::imgui_backend_interface* backend_)
	: backend(backend_)
{
}

gui::gui_system::~gui_system()
{
}

void gui::gui_system::render_menues()
{
	if (is_hiden()) {
		return;
	}

	for (auto layer : layers) {
		layer->on_update(std::chrono::milliseconds(15));
	}
}

void gui::gui_system::set_is_input_enabled(bool enable)
{
	if (is_input_enabled == enable) {
		return;
	}

	is_input_enabled = enable;
}

void gui::gui_system::render()
{
	PROFILE_SCOPE("GuiRender");
	if (m_pending_scale > 0.f) {
		backend->rebuild_fonts(m_pending_scale);
		m_pending_scale = 0.f;
	}
	backend->new_frame();
	render_menues();
	backend->render();
}
