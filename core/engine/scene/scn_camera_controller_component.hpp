#pragma once
#include <common.h>

namespace scn
{
	struct mouse_controller_component
	{
		float movement_speed = 1.f;
		float rotating_speed = glm::radians(90.f);
		float distance = 2;
		glm::vec3 rotation{ 0 };
		glm::vec3 position{ 0 };
		glm::vec2 last_mouse_pos{ 0 };
		bool is_moving = false;
		bool is_rotating = false;
	};
}