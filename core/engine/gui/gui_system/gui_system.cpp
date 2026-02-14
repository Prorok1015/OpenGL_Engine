#include "gui_system.h"
#include "engine_assert.h"
#include "rnd_render_system.h"
#include "wnd_window_system.h"

gui::gui_system* p_gui_system = nullptr;

gui::gui_system& gui::get_system()
{
	ASSERT_MSG(p_gui_system, "DebugUi system is nullptr!");
	return *p_gui_system;
}

gui::gui_system::gui_system(gui::imgui_backend_interface* backend_)
	: backend(backend_)
{
	renderer = std::make_shared<gui::renderer>(backend);
	renderer->render_event += [this] { render_menues(); };
	rnd::get_system().activate_renderer(renderer);
}

gui::gui_system::~gui_system()
{
	rnd::get_system().deactivate_renderer(renderer);
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

//void gui::gui_system::switch_input(inp::KEYBOARD_BUTTONS code, inp::KEY_ACTION action)
//{
//	if (code == inp::KEYBOARD_BUTTONS::F5 && action == inp::KEY_ACTION::UP)
//	{
//		gs::get_system().set_enable_input(layout.get_is_show_main_menu());
//		layout.set_show_main_menu(!layout.get_is_show_main_menu());
//		ImGui::SetWindowFocus();
//	}
//}

void gui::gui_system::set_is_input_enabled(bool enable)
{
	if (is_input_enabled == enable) {
		return;
	}

	is_input_enabled = enable;
}