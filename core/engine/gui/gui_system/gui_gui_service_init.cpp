#include <snk/gui/gui_gui_service_init.h>
#include <snk/gui/gui_system.h>
#include <snk/window/wnd_window_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/gui/gui_debug_layer.h>
#include <snk/gui_gl/gui_gl_backend.h>
#include <snk/gui/gui_null_backend.h>
#include <snk/gui/gui_theme.h>
#include <snk/gui/gui_platform_backend.h>
#include <snk/application/app_deterministic_time.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_binary.h>
#include <snk/common/fs_paths.h>
#include <snk/config/cfg_variables.h>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace {
// Where the window layout (ImGui's ini) is loaded from and saved to; empty: it is not kept on disk, so a process writes no
// file into the folder it was started from (#713). The editor sets a default in the user's folder (editor::apply_default_settings).
CFG_VAR_DEF_PATH(cfg_gui_layout_file, "gui/layout_file", "");
// classic | dark_neutral | dark_contrast | light (#1109); classic until the owner picks a look.
CFG_VAR_DEF_STRING(cfg_gui_theme, "gui/theme", "classic");
// auto (Noto for classic, Inter for the others) | noto | inter: lets the snapshots compare one theme on both fonts.
CFG_VAR_DEF_STRING(cfg_gui_font, "gui/font", "auto");

constexpr std::string_view noto_font = "fonts/NotoSans-Regular.ttf";
constexpr std::string_view inter_font = "fonts/Inter-Regular.ttf";
constexpr std::string_view icon_font = "fonts/lucide-icons.ttf";

// A font load already started, so the editor's fonts load side by side and the main thread waits once.
struct font_request {
	res::tag tag;
	std::optional<res::res_handle<res::binary_resource>> handle;
};

font_request request_engine_font(res::resource_system& resources, std::string_view file)
{
	font_request request{resources.engine_file(file), std::nullopt};
	if (resources.exists(request.tag)) {
		request.handle = resources.require<res::binary_resource>(request.tag);
	} else {
		egLOG_WARN("gui", "font '{}' not found, using the built-in ImGui font", request.tag.view());
	}
	return request;
}

std::vector<std::byte> bytes_of(const font_request& request)
{
	if (!request.handle) {
		return {};
	}
	auto font = request.handle->get_sync();
	if (!font) {
		egLOG_WARN("gui", "font '{}' failed to load, using the built-in ImGui font", request.tag.view());
		return {};
	}
	return font->bytes();
}

::gui::theme configured_theme()
{
	if (auto t = ::gui::theme_from_name(*cfg_gui_theme)) {
		return *t;
	}
	egLOG_WARN("gui", "unknown gui/theme '{}', using classic", *cfg_gui_theme);
	return ::gui::theme::classic;
}

std::string_view text_font_of(::gui::theme t)
{
	if (*cfg_gui_font == "noto") {
		return noto_font;
	}
	if (*cfg_gui_font == "inter") {
		return inter_font;
	}
	if (*cfg_gui_font != "auto") {
		egLOG_WARN("gui", "unknown gui/font '{}', using auto", *cfg_gui_font);
	}
	return t == ::gui::theme::classic ? noto_font : inter_font;
}
}

std::vector<std::byte> engine::gui::load_ui_font_bytes(res::resource_system& resources)
{
	return bytes_of(request_engine_font(resources, noto_font));
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
		auto& gl_backend = static_cast<::gui::gl::gl_imgui_backend&>(
			data.construct<::gui::imgui_backend_interface, ::gui::gl::gl_imgui_backend>(*driver));
		auto& resources = data.require<res::resource_system>();
		const ::gui::theme theme = configured_theme();
		const font_request text = request_engine_font(resources, text_font_of(theme));
		// Classic keeps the look the smoke references were taken with, so it gets no icons.
		const font_request icons = theme == ::gui::theme::classic
			? font_request{}
			: request_engine_font(resources, icon_font);
		gl_backend.set_theme(theme);
		gl_backend.set_ui_font_bytes(bytes_of(text));
		gl_backend.set_icon_font_bytes(bytes_of(icons));
		backend_ptr = &gl_backend;
	} else {
		backend_ptr = &data.construct<::gui::imgui_backend_interface, ::gui::null_imgui_backend>(*driver);
	}
	auto& backend = *backend_ptr;
	backend.set_dpi_scale(wnd_system.get_active_content_scale());
	// Before init(), which loads the layout from it.
	backend.set_settings_filename(fs_paths::utf8_of(*cfg_gui_layout_file));
	backend.init();
	std::error_code layout_ec;
	const auto is_exist = !cfg_gui_layout_file->empty() && std::filesystem::exists(*cfg_gui_layout_file, layout_ec);
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
