#pragma once
#include "res_tag.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace edt {

	// Marker component — distinguishes editor camera entity from scene cameras
	struct editor_camera_tag {};

	// Persistent state (survives hot-reload)
	struct editor_camera_state {
		glm::vec3 position      { 0.f, 2.f, 5.f };
		glm::vec3 rotation      { 0.f, 45.f, 0.f };           // pitch/yaw/roll radians
		float     distance      = 1.f;
		float     movement_speed  = 3.f;
		float     rotating_speed  = glm::radians(90.f);
		float     fov             = 60.f;
		float     near_distance   = 0.01f;
		float     far_distance    = 1000.f;

		// Render target tag for editor camera
		static constexpr std::string_view RT_TAG = "memory://__editor_camera_rt";
	};

} // namespace edt
