#pragma once
#include <snk/core_input/inp_keyboard_event.hpp>
#include <vector>

namespace inp
{
	using KEYBOARD_BUTTONS = core::inp::KEYBOARD_BUTTONS;
	using MOUSE_BUTTONS = core::inp::MOUSE_BUTTONS;
	using KEY_ACTION = core::inp::KEY_ACTION;

	struct input_state {
		std::array<bool, (std::underlying_type_t<KEYBOARD_BUTTONS>)KEYBOARD_BUTTONS::COUNT> keyboard{ false };
		std::array<bool, (std::underlying_type_t<MOUSE_BUTTONS>)MOUSE_BUTTONS::COUNT> mouse{ false };
		glm::vec2 mouse_pos{};
		glm::vec2 mouse_prev{};
		glm::vec2 mouse_dir{};
		glm::vec2 scroll{};
	};

	// Per-world flag: set by viewport panels to gate camera input.
	// Default active=true so worlds without an explicit panel still work.
	struct input_focus { bool active = true; };
}