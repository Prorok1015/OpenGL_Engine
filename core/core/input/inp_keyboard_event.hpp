#pragma once
#include "inp_input_event.hpp"

namespace core::inp
{
	enum class KEY_ACTION : uint8_t
	{
		NONE,
		UP,
		DOWN,
	};

	enum class KEYBOARD_BUTTONS : uint8_t
	{
		A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
		SPACE, APOSTROPHE, COMMA, MINUS, PERIOD, SLASH,
		K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9,
		SEMICOLON, EQUAL,
		ESCAPE, ENTER, TAB, BACKSPACE, INSERT, DELETE,
		RIGHT, LEFT, DOWN, UP, PAGE_UP, PAGE_DOWN, HOME, END,
		CAPS_LOCK, SCROLL_LOCK, NUM_LOCK, PRINT_SCREEN, PAUSE,
		F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24, F25,
		KP_0, KP_1, KP_2, KP_3, KP_4, KP_5, KP_6, KP_7, KP_8, KP_9, KP_DECIMAL, KP_DIVIDE, KP_MULTIPLY, KP_SUBTRACT, KP_ADD, KP_ENTER, KP_EQUAL,
		LEFT_SHIFT, LEFT_CONTROL, LEFT_ALT, LEFT_SUPER, RIGHT_SHIFT, RIGHT_CONTROL, RIGHT_ALT, RIGHT_SUPER, MENU
	};

	enum class MOUSE_BUTTONS : uint8_t
	{
		LEFT,
		RIGHT,
		MIDDLE,

		BTN_1 = LEFT,
		BTN_2 = RIGHT,
		BTN_3 = MIDDLE,
		BTN_4,
		BTN_5,
		BTN_6,
		BTN_7,
		BTN_8,
		LAST,
		COUNT
	};

	struct keyboard_event { KEYBOARD_BUTTONS key; KEY_ACTION action; };
	struct mouse_click_event { MOUSE_BUTTONS key; KEY_ACTION action; };
	struct cursor_move_event { glm::vec2 pos;};
	struct scroll_move_event { glm::vec2 direction; };
}