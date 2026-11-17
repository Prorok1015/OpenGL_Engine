#include <snk/window/wnd_native_key.h>
#include "wnd_input_keycode_convert.hpp"

std::optional<core::inp::KEYBOARD_BUTTONS> wnd::from_native_key(int native_key)
{
	return convert::to_keyboard_keycode(native_key);
}

int wnd::to_native_key(core::inp::KEYBOARD_BUTTONS key)
{
	return convert::to_glfw_keycode(key);
}
