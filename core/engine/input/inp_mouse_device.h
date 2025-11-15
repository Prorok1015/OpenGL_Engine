#pragma once
#include <common.h>
#include "inp_key_enums.hpp"
#include <glm/glm.hpp>

namespace inp
{

	class MouseDevice
	{
	public:
		glm::vec2 get_delta_pos() const { return prev - cur; }
		const glm::vec2& get_pos() const { return cur; }
		const glm::vec2& get_old_pos() const { return prev; }
		const glm::vec2& get_scroll() const { return scroll_direction; }
		glm::vec2 get_direction() const { return (prev - cur) / (glm::vec2(window_size) * 0.5f); }

		void on_window_resize(const glm::ivec2& size) { window_size = size; }
		void on_mouse_move(double xpos, double ypos);
		void on_mouse_button_action(MOUSE_BUTTONS button, KEY_ACTION action, int mode /* temporary unused */);
		void on_mouse_scroll(double xpos, double ypos);

		Key get_key(MOUSE_BUTTONS key) const;

		void clear_scroll() { scroll_direction = glm::zero<glm::vec2>(); }

		auto operator<=>(const MouseDevice&) const noexcept = default;

	private:
		glm::ivec2 window_size{ 0 };
		glm::vec2 scroll_direction{ 0 };
		glm::vec2 cur{ 0 };
		glm::vec2 prev{ 0 };
		std::array<KEY_ACTION, MOUSE_BUTTONS_COUNT> keys_{};
	};
}
