#pragma once
#include <snk/resource/res_tag.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <entt/entity/fwd.hpp>

namespace edt {

	// Marker component — distinguishes editor camera entity from scene cameras
	struct editor_camera_tag {};

	// Persistent state (survives hot-reload)
	struct editor_camera_state {
		// The default pose is the identity matrix: the camera renders from the origin until it moves, and
		// the pose it is rebuilt from after a world reload must be the same one.
		glm::vec3 position      { 0.f, 0.f, 0.f };
		glm::vec3 rotation      { 0.f, 0.f, 0.f };            // pitch/yaw/roll radians
		float     distance      = 0.f;
		float     movement_speed  = 3.f;
		float     rotating_speed  = glm::radians(90.f);
		float     fov             = 60.f;
		float     near_distance   = 0.01f;
		float     far_distance    = 1000.f;

		// Render target tag for editor camera
		static constexpr std::string_view RT_TAG = "memory://__editor_camera_rt";
	};

	// Copies the pose of the editor camera entity of `reg` into `state` (no-op when there is none).
	void save_camera_state(entt::registry& reg, editor_camera_state& state);
	// Replaces the editor camera entity of `reg` with one built from `state`.
	void inject_camera(entt::registry& reg, const editor_camera_state& state);

} // namespace edt
