#pragma once
#include <optional>

#include <snk/common/snk_api.h>
#include <snk/core_input/inp_keyboard_event.hpp>

namespace wnd
{
	// Window-backend key tokens (GLFW key codes today) <-> engine keys. Tokens are physical key positions.
	// from_native_key: nullopt for a token with no engine key (unknown key). to_native_key: -1 for COUNT.
	SNK_API std::optional<core::inp::KEYBOARD_BUTTONS> from_native_key(int native_key);
	SNK_API int to_native_key(core::inp::KEYBOARD_BUTTONS key);
}
