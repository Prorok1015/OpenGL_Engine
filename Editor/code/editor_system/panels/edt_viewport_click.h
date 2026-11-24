#pragma once
#include "edt_mesh_gesture.h"
#include <glm/glm.hpp>

namespace edt
{
	struct viewport_click_frame
	{
		bool takes_input = false;
		bool locked = false;
		bool pressed = false;
		bool down = false;
		glm::vec2 pixel{ 0.f };
		mesh::key_modifiers modifiers;
	};

	// Left button on the viewport image: a click selects, a drag past the threshold is a box (#1327), Alt+left pans the camera.
	class viewport_click
	{
	public:
		explicit viewport_click(float drag_threshold) : m_gesture(drag_threshold) {}

		mesh::gesture_result update(const viewport_click_frame& frame);
		bool camera_input() const { return m_gesture.camera_input(); }
		const mesh::select_gesture& gesture() const { return m_gesture; }

	private:
		mesh::select_gesture m_gesture;
	};
}
