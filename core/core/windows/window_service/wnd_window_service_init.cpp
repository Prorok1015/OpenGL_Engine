#include <snk/window/wnd_window_service_init.h>
#include <snk/window/wnd_window_system.h>
#include <snk/window/wnd_null_window_system.h>
#include <snk/config/cfg_variables.h>
#include <snk/common/logger/engine_log.h>
#include "wnd_glfw_window_system.h"

namespace {
	// "glfw" (default) or "null": no display, no GL surface, so the renderer falls back to null too (--headless).
	CFG_VAR_DEF_STRING(cfg_window_system, "window/system", "glfw");
}

void core::window::window_init(ds::app_data_storage& data)
{
	const std::string& system = *cfg_window_system;
	if (system == "null") {
		egLOG_INFO("window", "Window system: null (window/system = null)");
		data.construct<wnd::window_system, wnd::null_window_system>();
		return;
	}
	if (system != "glfw") {
		egLOG_WARN("window", "Unknown window/system '{}', expected 'glfw' or 'null'; using glfw", system);
	}
	data.construct<wnd::window_system, wnd::glfw_window_system>();
}

void core::window::window_term(ds::app_data_storage& data)
{
	data.destruct<wnd::window_system>();
}
