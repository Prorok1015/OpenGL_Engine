#pragma once
#include <common.h>
#include "wnd_input_enums.hpp"

namespace inp
{
	using KEY_ACTION = wnd::KEY_ACTION;
	using KEYBOARD_BUTTONS = wnd::KEYBOARD_BUTTONS;
	using MOUSE_BUTTONS = wnd::MOUSE_BUTTONS;

	constexpr std::size_t MOUSE_BUTTONS_COUNT = (std::size_t)MOUSE_BUTTONS::COUNT;

	struct Key
	{
		KEY_ACTION action = KEY_ACTION::NONE;
		float time_stamp_down = 0.f;
		float time_stamp_up = 0.f;
	};

}
 