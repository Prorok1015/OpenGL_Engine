#include "inp_input_system.h"

void input::InputSystem::end_frame()
{
	glfwPollEvents();

	keyboard.visit_keys(onKeyAction);
}

const input::Key& input::InputSystem::get_key_state(KEYBOARD_BUTTONS key) const
{
	return keyboard.get_key(key);
}

const input::Key& input::InputSystem::get_key_state(MOUSE_BUTTONS key) const
{
	return mouse.get_key(key);
}