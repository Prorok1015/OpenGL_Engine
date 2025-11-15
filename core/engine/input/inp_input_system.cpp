#include "inp_input_system.h"
#include <engine_log.h>

inp::input_system* p_inp_system = nullptr;

inp::input_system& inp::get_system()
{
	ASSERT_MSG(p_inp_system, "Input system is nullptr!");
	return *p_inp_system;
}

inp::input_system::input_system()
{
	keyboard.onKeyStateChanged += [this](auto a, auto b) { onKeyAction(a, b); };

}

inp::input_system::~input_system()
{
}

void inp::input_system::process_input(float dt)
{
	while (!event_queque.empty()) {
		auto& evt = event_queque.front();

		for (auto weak_inp_mng : input_managers_list)
		{
			if (auto inp_mng = weak_inp_mng.lock()) {
				if (inp_mng->on_handle_event(evt)) {
					break;
				}
			}
		}

		event_queque.pop();
	}

	for (auto weak_inp_mng : input_managers_list)
	{
		if (auto inp_mng = weak_inp_mng.lock()) {
			inp_mng->on_notify_listeners(dt);
		}
	}

}

void inp::input_system::activate_manager(std::weak_ptr<input_manager_base> inp_manager)
{
	auto pred = [](auto& lhs, auto& rhs) {
		auto lhs_r = lhs.lock();
		auto rhs_r = rhs.lock();
		if (!lhs_r || !rhs_r) {
			return false;
		}

		return lhs_r->get_priority() < rhs_r->get_priority();
		};

	input_managers_list.insert(std::lower_bound(input_managers_list.begin(), input_managers_list.end(), inp_manager, pred), inp_manager);
}

void inp::input_system::deactivate_manager(std::weak_ptr<input_manager_base> inp_manager)
{
	auto pred = [find = inp_manager.lock()](auto& lhs) {
		auto lhs_r = lhs.lock();
		if (!lhs_r) {
			return false;
		}

		return lhs_r == find;
	};

	auto it = std::find_if(input_managers_list.begin(), input_managers_list.end(), pred);
	input_managers_list.erase(it);
}


inp::Key inp::input_system::get_key_state(KEYBOARD_BUTTONS key) const
{
	return keyboard.get_key(key);
}

inp::Key inp::input_system::get_key_state(MOUSE_BUTTONS key) const
{
	return mouse.get_key(key);
}


void inp::input_system::on_keyboard_event(const keyboard_event& evt)
{
	event_queque.push(evt);
}

void inp::input_system::on_mouse_buttons_event(const mouse_click_event& evt)
{
	event_queque.push(evt);
}

void inp::input_system::on_cursor_move_event(const cursor_move_event& evt)
{
	event_queque.push(evt);
}

void inp::input_system::on_scroll_move_event(const scroll_move_event& evt)
{
	event_queque.push(evt);
}