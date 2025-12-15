#include "edt_input_manager.h"
#include "wnd_keyboard_event.hpp"

bool edt::input_manager::on_handle_event(wnd::handle, const wnd::input_event& evt)
{
    if (auto* c_evt = evt.get_payload<wnd::cursor_move_event>()) {
        last_cursor_pos = c_evt->pos;
    }

	if (input_area != glm::zero<glm::ivec4>()) {
	    bool is_handling = true;

        if (invert) {
            is_handling = (last_cursor_pos.x < input_area.x || last_cursor_pos.x > input_area.z ||
                            last_cursor_pos.y < input_area.y || last_cursor_pos.y > input_area.w);
        } else {
            is_handling = (last_cursor_pos.x >= input_area.x && last_cursor_pos.x <= input_area.z &&
                            last_cursor_pos.y >= input_area.y && last_cursor_pos.y <= input_area.w);
        }

        if (!is_handling) {
            return false;
        }
    }

    if (auto* mouse_clk = evt.get_payload<wnd::mouse_click_event>()) {
		return mouse_clk->action == wnd::KEY_ACTION::DOWN;
	}
	else if (auto* kbd_evt = evt.get_payload<wnd::keyboard_event>()) {
		return kbd_evt->action == wnd::KEY_ACTION::DOWN;
	}
	else if (auto* c_evt = evt.get_payload<wnd::cursor_move_event>()) {
		last_cursor_pos = c_evt->pos;
	}

    return false;
}
