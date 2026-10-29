#pragma once
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>

namespace core::window {
	// Builds the window system that config `window/system` names (glfw or null).
	SNK_API void window_init(ds::app_data_storage& data);
	SNK_API void window_term(ds::app_data_storage& data);
}