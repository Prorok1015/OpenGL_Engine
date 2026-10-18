#include "gui_system.h"
#include "gui_platform_backend.h"
#include <imgui.h>
#include "engine_assert.h"
#include "rnd_render_system.h"
#include "wnd_window_system.h"
#include "eng_profiler.h"

gui::gui_system::gui_system(gui::imgui_backend_interface* backend_, gui::platform_backend* platform_)
	: backend(backend_)
	, m_platform(platform_)
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
		if (m_pending_scale != m_applied_scale) {
			backend->rebuild_fonts(m_pending_scale);
			m_applied_scale = m_pending_scale;
		}
		m_pending_scale = 0.f;
	}
	backend->new_frame();
	m_platform->new_frame();
	ImGui::NewFrame();
	// DockSpace is managed at the editor level (edt_dockspace)
	render_menues();
	backend->render();
}
