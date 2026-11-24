#include "edt_viewport_click.h"

namespace edt
{
	mesh::gesture_result viewport_click::update(const viewport_click_frame& frame)
	{
		if (frame.locked) {
			m_gesture.cancel();
			return {};
		}
		if (m_gesture.active()) {
			if (frame.interrupted || (!frame.down && !frame.released)) {
				m_gesture.cancel();
				return {};
			}
			if (frame.down) {
				m_gesture.move(frame.pixel);
				return {};
			}
			return m_gesture.end(frame.pixel);
		}
		if (frame.takes_input && frame.pressed)
			m_gesture.begin(frame.pixel, frame.modifiers);
		return {};
	}
}
