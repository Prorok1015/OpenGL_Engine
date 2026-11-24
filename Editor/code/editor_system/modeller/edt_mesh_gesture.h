#pragma once
#include "edt_mesh_picking.h"
#include "edt_mesh_selection.h"
#include <cstdint>
#include <glm/glm.hpp>

namespace edt::mesh
{
	struct key_modifiers
	{
		bool shift = false;
		bool ctrl = false;
		bool alt = false;
	};

	// Ctrl toggles, Shift adds, neither replaces (Ctrl wins over Shift).
	select_op select_op_of(const key_modifiers& modifiers);

	enum class gesture_end : std::uint8_t
	{
		none,
		click,
		box,
	};

	struct gesture_result
	{
		gesture_end kind = gesture_end::none;
		select_op op = select_op::replace;
		// click: where it was; box: both corners.
		glm::vec2 point{ 0.f };
		pixel_rect rect;
	};

	// The left button over the preview image, with no ImGui: a press that moves less than the threshold is a click, one that moves
	// more is a box; with Alt the press is no selection at all and the camera pans. While a selection gesture runs the camera must
	// not see the mouse (`camera_input()` is false): the engine's camera controller moves on a left drag, and a click would shift the view.
	class select_gesture
	{
	public:
		// `drag_threshold` in pixels, above zero.
		explicit select_gesture(float drag_threshold = 4.f);

		// A press that comes while a gesture runs is ignored.
		void begin(const glm::vec2& pixel, const key_modifiers& modifiers);
		void move(const glm::vec2& pixel);
		gesture_result end(const glm::vec2& pixel);
		void cancel();
		// Called every frame with whether the button is down: a release that was lost (the window lost the focus, the button came up
		// outside the image) ends the gesture without a result, so the camera is never left locked. True when it ended one.
		bool release_lost(bool button_down);

		bool active() const { return m_phase != phase::idle; }
		bool selecting() const { return m_phase == phase::select; }
		bool dragging_box() const { return m_phase == phase::select && m_dragging; }
		// The box so far, while dragging_box(), with its corners in order.
		pixel_rect box() const { return { glm::min(m_press, m_current), glm::max(m_press, m_current) }; }
		// Whether the camera controller may read the mouse now.
		bool camera_input() const { return m_phase != phase::select; }

	private:
		enum class phase : std::uint8_t
		{
			idle,
			select,
			pan,
		};

		float m_threshold = 4.f;
		phase m_phase = phase::idle;
		bool m_dragging = false;
		glm::vec2 m_press{ 0.f };
		glm::vec2 m_current{ 0.f };
		select_op m_op = select_op::replace;
	};
}
