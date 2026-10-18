#include "gui_gui_service_init.h"
#include "gui_system.h"
#include "wnd_window_system.h"
#include "gui_debug_layer.h"
#include "gui_gl_backend.h"
#include <filesystem>

void engine::gui::gui_init(ds::app_data_storage& data)
{
	auto& wnd_system = data.require<wnd::window_system>();
	// The GUI layer now owns the imgui backend (was owned by window — EPIC-49 US-49-9). It lives
	// in app storage; window only provides the native handle + DPI scale. The GL context already
	// exists (created by window_system in core_module init, which runs before this engine init).
	auto& backend = data.construct<::gui::gl::gl_imgui_backend>();
	backend.set_dpi_scale(wnd_system.get_active_content_scale());
	backend.init(wnd_system.get_active_native_handle());
	const auto is_exist = std::filesystem::exists(backend.get_settings_filename());
	backend.set_initial_layout_by_default(!is_exist);

	auto& gui_sys = data.construct<::gui::gui_system>(&backend);
	gui_sys.push_layer(std::make_shared<::gui::debug_layer>());
}

void engine::gui::gui_term(ds::app_data_storage& data)
{
	data.destruct<::gui::gui_system>();
	// Shut the backend down while the GL context is still alive (engine terminates before core).
	data.require<::gui::gl::gl_imgui_backend>().shutdown();
	data.destruct<::gui::gl::gl_imgui_backend>();
}
