#pragma once
#include "inp_keyboard_event.hpp"
#include <vector>

namespace inp
{
	using KEYBOARD_BUTTONS = core::inp::KEYBOARD_BUTTONS;
	using MOUSE_BUTTONS = core::inp::MOUSE_BUTTONS;
	using KEY_ACTION = core::inp::KEY_ACTION;

	struct keyboard_event { KEYBOARD_BUTTONS key; KEY_ACTION action; };
	struct mouse_click_event { MOUSE_BUTTONS key; KEY_ACTION action; glm::vec2 pos; };
	struct cursor_move_event { glm::vec2 pos; glm::vec2 prev; glm::vec2 direction; };
	struct scroll_move_event { glm::vec2 direction; };

	struct input_state {
		std::array<bool, (std::underlying_type_t<KEYBOARD_BUTTONS>)KEYBOARD_BUTTONS::COUNT> keyboard{ false };
		std::array<bool, (std::underlying_type_t<MOUSE_BUTTONS>)MOUSE_BUTTONS::COUNT> mouse{ false };
		glm::vec2 mouse_pos{};
		glm::vec2 mouse_prev{};
		glm::vec2 mouse_dir{};
		glm::vec2 scroll{};
	};
}