#include "edt_input_manager.h"
#include "wnd_keyboard_event.hpp"

namespace {
    struct event_visitor
    {
        bool operator() (const inp::keyboard_event& evt) const noexcept {
            return evt.action == inp::KEY_ACTION::DOWN;
        }
        bool operator() (const inp::mouse_click_event& evt) const noexcept {
            return evt.action == inp::KEY_ACTION::DOWN;
        }
        bool operator() (const inp::scroll_move_event& evt) const noexcept {
            return true;
        }

        bool operator() (const auto&) const noexcept {
            return false;
        }
    };
}

bool edt::input_manager::on_handle_event(const inp::input_event& evt)
{
    return std::visit(event_visitor(), evt);
}

void edt::input_manager::on_notify_listeners(float dt)
{
    notify_listeners(dt);
}

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

    //if (!is_block_keyaction_down || !is_block_keyaction_down_once) {
    //    return false;
    //}

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
    //return true;
}
