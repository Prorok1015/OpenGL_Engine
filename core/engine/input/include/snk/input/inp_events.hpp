#pragma once
#include <snk/core_input/inp_keyboard_event.hpp>
#include <glm/glm.hpp>
#include <array>
#include <type_traits>
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

	// ctx resource written by a UI context's systems: where on screen the context takes the mouse
	// (rects {x0, y0, x1, y1} in framebuffer pixels, bounds included, from its layout of the
	// previous frame) and whether it takes the keyboard (a focused text field, a modal).
	// gs::route_input reads it; contexts without it capture nothing.
	struct input_capture
	{
		std::vector<glm::ivec4> regions;
		bool keyboard = false;
	};
}