#include <snk/gui/gui_gui_service_init.h>
#include <snk/gui/gui_system.h>
#include <snk/window/wnd_window_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/gui/gui_debug_layer.h>
#include <snk/gui_gl/gui_gl_backend.h>
#include <snk/gui/gui_null_backend.h>
#include <snk/gui/gui_platform_backend.h>
#include <snk/application/app_deterministic_time.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_binary.h>
#include <filesystem>
#include <vector>

namespace {
// The shipped UI font (core/res, base layer). Not an error when it is absent: the backend keeps the built-in
// ImGui font, so the editor starts and looks as it did before the font was added.
std::vector<std::byte> load_ui_font_bytes(res::resource_system& resources)
{
	const res::tag font_tag{ "res://fonts/NotoSans-Regular.ttf" };
	if (!resources.exists(font_tag)) {
		egLOG_WARN("gui", "UI font '{}' not found, using the built-in ImGui font (no Cyrillic)", font_tag.view());
		return {};
	}
	auto font = resources.require_sync<res::binary_resource>(font_tag).get_sync();
	if (!font) {
		egLOG_WARN("gui", "UI font '{}' failed to load, using the built-in ImGui font (no Cyrillic)", font_tag.view());
		return {};
	}
	return font->bytes();
}
}

void engine::gui::gui_init(ds::app_data_storage& data)
{
	auto& wnd_system = data.require<wnd::window_system>();
	// The GUI layer owns both ImGui halves; only this function names the concrete backends.
	// The GL context already exists (created by window_system in core_module init, which runs
	// before this engine init).
	// The panels show render targets: the backend turns their texture handles into ImGui ids through the driver.
	auto* driver = data.require<rnd::render_system>().get_driver();
	ASSERT_MSG(driver, "gui_init: the render system has no driver");
	auto window = wnd_system.get_active_window();
	ASSERT_MSG(window, "gui_init: no active window");
	// Without a GL surface (null window, --headless) the render backend is null too: nothing to draw ImGui with.
	::gui::imgui_backend_interface* backend_ptr = nullptr;
	if (window && window->gl_surface_ptr()) {
		auto& gl_backend = data.construct<::gui::imgui_backend_interface, ::gui::gl::gl_imgui_backend>(*driver);
		static_cast<::gui::gl::gl_imgui_backend&>(gl_backend).set_ui_font_bytes(load_ui_font_bytes(data.require<res::resource_system>()));
		backend_ptr = &gl_backend;
	} else {
		backend_ptr = &data.construct<::gui::imgui_backend_interface, ::gui::null_imgui_backend>(*driver);
	}
	auto& backend = *backend_ptr;
	backend.set_dpi_scale(wnd_system.get_active_content_scale());
	backend.init();
	const auto is_exist = std::filesystem::exists(backend.get_settings_filename());
	backend.set_initial_layout_by_default(!is_exist);

	auto& platform = data.construct<::gui::platform_backend>(*window);
	platform.init();
	if (data.has_value<app::deterministic_time>()) {
		platform.set_fixed_delta(data.require<app::deterministic_time>().fixed_dt);
	}
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
	data.require<::gui::imgui_backend_interface>().shutdown();
	data.destruct<::gui::imgui_backend_interface>();
}
