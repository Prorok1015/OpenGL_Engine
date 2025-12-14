#pragma once
#include "wnd_input_enums.hpp"

namespace wnd
{
	struct keyboard_event { KEYBOARD_BUTTONS key; KEY_ACTION action; };
	struct mouse_click_event { MOUSE_BUTTONS key; KEY_ACTION action; };
	struct cursor_move_event { glm::vec2 pos;};
	struct scroll_move_event { glm::vec2 direction; };


}