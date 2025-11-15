#include "inp_input_actions.h"

void inp::InputActionMouseMove::update(float dt)
{
	input_system& inpSys = inp::get_system();
	glm::vec2 current_position = inpSys.mouse.get_pos();

	if (last_position != current_position) {
		on_action(current_position, last_position);
		last_position = current_position;
	}
}

void inp::InputMouseWhell::update(float dt)
{
	input_system& inpSys = inp::get_system();
	glm::vec2 current_position = inpSys.mouse.get_scroll();

	if (last_position != current_position) {
		on_action(current_position, last_position);
		last_position = current_position;
	}
}
