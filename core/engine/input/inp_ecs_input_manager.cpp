#include "inp_ecs_input_manager.h"
#include "inp_keyboard_event.hpp"
#include "inp_events.hpp"

bool inp::ecs_input_manager::on_handle_event(wnd::handle win, const inp::input_event& evt)
{
	if (input_area != glm::zero<glm::ivec4>()) {
		bool is_handling = true;

		if (invert) {
			is_handling = (input_state.mouse_pos.x < input_area.x || input_state.mouse_pos.x > input_area.z ||
						   input_state.mouse_pos.y < input_area.y || input_state.mouse_pos.y > input_area.w);
		} else {
			is_handling = (input_state.mouse_pos.x >= input_area.x && input_state.mouse_pos.x <= input_area.z &&
						   input_state.mouse_pos.y >= input_area.y && input_state.mouse_pos.y <= input_area.w);
		}

		if (!is_handling) {
			if (auto* c_evt = evt.get_payload<core::inp::cursor_move_event>()) {
				input_state.mouse_pos = c_evt->pos;
			}
			if (const auto* payload = evt.get_payload<core::inp::mouse_click_event>()) {
				input_state.mouse[(std::underlying_type_t<inp::MOUSE_BUTTONS>)payload->key] = payload->action == core::inp::KEY_ACTION::DOWN;
			}
			return false;
		}
	} else if (invert) {
		if (auto* c_evt = evt.get_payload<core::inp::cursor_move_event>()) {
			input_state.mouse_pos = c_evt->pos;
		}
		if (const auto* payload = evt.get_payload<core::inp::mouse_click_event>()) {
			input_state.mouse[(std::underlying_type_t<inp::MOUSE_BUTTONS>)payload->key] = payload->action == core::inp::KEY_ACTION::DOWN;
		}
		return false;
	}

	if (const auto* payload = evt.get_payload<core::inp::keyboard_event>()) {
		input_state.keyboard[(std::underlying_type_t<inp::KEYBOARD_BUTTONS>)payload->key] = payload->action == core::inp::KEY_ACTION::DOWN;
	} else if (const auto* payload = evt.get_payload<core::inp::mouse_click_event>()) {
		input_state.mouse[(std::underlying_type_t<inp::MOUSE_BUTTONS>)payload->key] = payload->action == core::inp::KEY_ACTION::DOWN;
	} else if (const auto* payload = evt.get_payload<core::inp::cursor_move_event>()) {
		if (input_area != glm::zero<glm::ivec4>()) {
			if (payload->pos.x < input_area.x || payload->pos.x > input_area.z ||
				payload->pos.y < input_area.y || payload->pos.y > input_area.w) {
				return false;
			}
		}

		glm::vec2 window_size = glm::vec2{input_area.z - input_area.x, input_area.w - input_area.y};

		input_state.mouse_prev = input_state.mouse_pos;
		input_state.mouse_pos = payload->pos;
		input_state.mouse_dir = (input_state.mouse_prev - input_state.mouse_pos) / (window_size * 0.5f);
	} else if (const auto* payload = evt.get_payload<core::inp::scroll_move_event>()) {
		input_state.scroll = payload->direction;
	}
	return false;
}
