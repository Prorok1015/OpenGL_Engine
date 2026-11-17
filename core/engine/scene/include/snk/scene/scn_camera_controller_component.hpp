#pragma once
#include <snk/common/common.h>

namespace scn
{
	struct mouse_controller_component
	{
		float movement_speed = 3.f;
		float rotating_speed = glm::radians(90.f);
		float distance = 2;
		glm::vec3 rotation{ 0 };
		glm::vec3 position{ 0 };
		glm::vec2 last_mouse_pos{ 0 };
		bool is_moving = false;
		bool is_rotating = false;
		// The owner of the camera (the editor) turns this off while the input is not meant for it, e.g. while another panel
		// is the one under the mouse. A disabled controller neither reads the input nor writes the transform.
		bool input_enabled = true;
	};
}