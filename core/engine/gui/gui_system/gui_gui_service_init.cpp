#include "gui_gui_service_init.h"
#include "gui_system.h"
#include <snk/window/wnd_window_system.h>
#include "gui_debug_layer.h"
#include "gui_gl_backend.h"
#include "gui_platform_backend.h"
#include "engine_assert.h"
#include <filesystem>

void engine::gui::gui_init(ds::app_data_storage& data)
{
	auto& wnd_system = data.require<wnd::window_system>();
	// The GUI layer owns both ImGui halves; only this function names the concrete backends.
	// The GL context already exists (created by window_system in core_module init, which runs
	// before this engine init).
	auto& backend = data.construct<::gui::gl::gl_imgui_backend>();
	backend.set_dpi_scale(wnd_system.get_active_content_scale());
	backend.init();
	const auto is_exist = std::filesystem::exists(backend.get_settings_filename());
	backend.set_initial_layout_by_default(!is_exist);

	auto window = wnd_system.get_active_window();
	ASSERT_MSG(window, "gui_init: no active window");
	auto& platform = data.construct<::gui::platform_backend>(*window);
	platform.init();
	// Added at init time, before the input system listener, so ImGui sees every event first; it
	// never consumes, routing stays with WantCapture*.
	wnd_system.add_event_listener(data.require_shared<::gui::platform_backend>());

	auto& gui_sys = data.construct<::gui::gui_system>(&backend, &platform);
	gui_sys.push_layer(std::make_shared<::gui::debug_layer>());
}

void engine::gui::gui_term(ds::app_data_storage& data)
{
	data.destruct<::gui::gui_system>();
	data.require<wnd::window_system>().remove_event_listener(data.require_shared<::gui::platform_backend>());
	// Platform before renderer: its shutdown touches the ImGui context the renderer destroys.
	// Shut down while the GL context is still alive (engine terminates before core).
	data.require<::gui::platform_backend>().shutdown();
	data.destruct<::gui::platform_backend>();
	data.require<::gui::gl::gl_imgui_backend>().shutdown();
	data.destruct<::gui::gl::gl_imgui_backend>();
}
