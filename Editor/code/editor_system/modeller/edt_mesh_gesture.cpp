#include "edt_mesh_gesture.h"

namespace edt::mesh
{
	select_op select_op_of(const key_modifiers& modifiers)
	{
		if (modifiers.ctrl) return select_op::toggle;
		if (modifiers.shift) return select_op::add;
		return select_op::replace;
	}

	void select_gesture::begin(const glm::vec2& pixel, const key_modifiers& modifiers)
	{
		if (m_phase != phase::idle) return;
		m_phase = modifiers.alt ? phase::pan : phase::select;
		m_dragging = false;
		m_press = pixel;
		m_current = pixel;
		m_op = select_op_of(modifiers);
	}

	void select_gesture::move(const glm::vec2& pixel)
	{
		if (m_phase != phase::select) return;
		m_current = pixel;
		if (!m_dragging && glm::length(m_current - m_press) >= m_threshold) m_dragging = true;
	}

	gesture_result select_gesture::end(const glm::vec2& pixel)
	{
		gesture_result result;
		if (m_phase == phase::select) {
			move(pixel);
			result.op = m_op;
			if (m_dragging) {
				result.kind = gesture_end::box;
				result.rect = box();
				result.point = m_current;
			} else {
				result.kind = gesture_end::click;
				result.point = m_press;
			}
		}
		cancel();
		return result;
	}

	void select_gesture::cancel()
	{
		m_phase = phase::idle;
		m_dragging = false;
	}
}
