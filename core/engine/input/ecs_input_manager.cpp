#include "ecs_input_manager.h"
#include "ecs_common_system.h"
#include "ecs_component.h"
#include "inp_keyboard_event.hpp"
#include "inp_events.hpp"

struct event_visitor
{
	event_visitor(ecs::flow_input_manager& mng_)
		: mng(mng_)
	{}

	void operator() (const auto& evt) const noexcept
	{
		using T = std::decay_t<decltype(evt)>;
		auto ent = mng.get_empty_entity();
		mng.get_registry().emplace_or_replace<T>(ent, evt);
		mng.get_registry().emplace_or_replace<ecs::input_changed_event_component>(ent);
	}

private:
	ecs::flow_input_manager& mng;
};

bool ecs::flow_input_manager::on_handle_event(wnd::handle win, const inp::input_event& evt)
{
	if (input_area != glm::zero<glm::ivec4>()) {
		bool is_handling = true;

		if (invert) {
			is_handling = (last_cursor_pos.x < input_area.x || last_cursor_pos.x > input_area.z ||
				last_cursor_pos.y < input_area.y || last_cursor_pos.y > input_area.w);
		}
		else {
			is_handling = (last_cursor_pos.x >= input_area.x && last_cursor_pos.x <= input_area.z &&
				last_cursor_pos.y >= input_area.y && last_cursor_pos.y <= input_area.w);
		}

		if (!is_handling) {
			if (auto* c_evt = evt.get_payload<core::inp::cursor_move_event>()) {
				last_cursor_pos = c_evt->pos;
			}
			if (const auto* payload = evt.get_payload<core::inp::mouse_click_event>()) {
				if (payload->action == core::inp::KEY_ACTION::UP){
					inp::mouse_click_event mouse_evt;
					mouse_evt.key = payload->key;
					mouse_evt.action = payload->action;
					mouse_evt.pos = last_cursor_pos;
					event_visitor{*this}(mouse_evt);
				}
			}
			return false;
		}
	} else if (invert) {
		if (auto* c_evt = evt.get_payload<core::inp::cursor_move_event>()) {
			last_cursor_pos = c_evt->pos;
		}
		if (const auto* payload = evt.get_payload<core::inp::mouse_click_event>()) {
			if (payload->action == core::inp::KEY_ACTION::UP) {
				inp::mouse_click_event mouse_evt;
				mouse_evt.key = payload->key;
				mouse_evt.action = payload->action;
				mouse_evt.pos = last_cursor_pos;
				event_visitor{ *this }(mouse_evt);
			}
		}
		return false;
	}

	event_visitor visitor(*this);

	if (const auto* payload = evt.get_payload<core::inp::keyboard_event>()) {
		inp::keyboard_event kbd_evt;
		kbd_evt.key = payload->key;
		kbd_evt.action = payload->action;
		visitor(kbd_evt);
	}
	else if (const auto* payload = evt.get_payload<core::inp::mouse_click_event>()) {
		inp::mouse_click_event mouse_evt;
		mouse_evt.key = payload->key;
		mouse_evt.action = payload->action;
		mouse_evt.pos = last_cursor_pos;
		visitor(mouse_evt);
	}
	else if (const auto* payload = evt.get_payload<core::inp::cursor_move_event>()) {
		if (input_area != glm::zero<glm::ivec4>()) {
			if (payload->pos.x < input_area.x || payload->pos.x > input_area.z ||
				payload->pos.y < input_area.y || payload->pos.y > input_area.w) {
				return false;
			}
		}

		glm::vec2 window_size = glm::vec2{input_area.z - input_area.x, input_area.w - input_area.y};
		glm::vec2 dir = (last_cursor_pos - glm::vec2{ input_area.x, input_area.y }) - (payload->pos - glm::vec2{ input_area.x, input_area.y });

		inp::cursor_move_event cursor_evt;
		cursor_evt.pos = payload->pos;
		cursor_evt.prev = last_cursor_pos;
		cursor_evt.direction = (last_cursor_pos - payload->pos) / (window_size * 0.5f);
		last_cursor_pos = payload->pos;
		visitor(cursor_evt);
	}
	else if (const auto* payload = evt.get_payload<core::inp::scroll_move_event>()) {
		inp::scroll_move_event scroll_evt;
		scroll_evt.direction = payload->direction;
		visitor(scroll_evt);
	}
	return false;
}
