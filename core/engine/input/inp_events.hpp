#pragma once
#include "common.h"
#include <variant>
#include "inp_key_enums.hpp"

namespace inp
{
	struct keyboard_event { KEYBOARD_BUTTONS key; KEY_ACTION action; };
	struct mouse_click_event { MOUSE_BUTTONS key; KEY_ACTION action; glm::vec2 pos; };
	struct cursor_move_event { glm::vec2 pos; glm::vec2 prev; glm::vec2 direction; };
	struct scroll_move_event { glm::vec2 direction; };

	using input_event = std::variant<keyboard_event, mouse_click_event, cursor_move_event, scroll_move_event>;
}