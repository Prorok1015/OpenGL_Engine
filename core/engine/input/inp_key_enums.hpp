#pragma once
#include "wnd_input_enums.hpp"

namespace inp
{
	using KEY_ACTION = wnd::KEY_ACTION;
	using KEYBOARD_BUTTONS = wnd::KEYBOARD_BUTTONS;
	using MOUSE_BUTTONS = wnd::MOUSE_BUTTONS;

	constexpr std::size_t MOUSE_BUTTONS_COUNT = (std::size_t)MOUSE_BUTTONS::COUNT;
}
 