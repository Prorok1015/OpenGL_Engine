#pragma once
#include <common.h>
#include <ds_type_id.hpp>
#include "inp_keyboard_device.h"
#include "inp_mouse_device.h"
#include "inp_input_manager_base.h"

namespace inp
{
	class input_system
	{
	public:
		input_system();
		~input_system();
		input_system(input_system&&) = delete;
		input_system& operator= (input_system&&) = delete;

		input_system(const input_system&) = delete;
		input_system& operator= (const input_system&) = delete;

		void process_input(float dt);

		void activate_manager(std::weak_ptr<input_manager_base> inp_manager);
		void deactivate_manager(std::weak_ptr<input_manager_base> inp_manager);

		// change
		void end_frame();

		Key get_key_state(KEYBOARD_BUTTONS) const;
		Key get_key_state(MOUSE_BUTTONS) const;

		template<class T, class HANDLER>
		void sub_keyboard_by_tag(HANDLER&& callback)
		{
			onKeyAction += callback;
		}

		template<class T>
		void unsub_keyboard_by_tag()
		{
			//onKeyAction.Unsubscribe(ds::Type::value<T>());
		}

		void on_keyboard_event(const keyboard_event&);
		void on_mouse_buttons_event(const mouse_click_event&);
		void on_cursor_move_event(const cursor_move_event&);
		void on_scroll_move_event(const scroll_move_event&);

		MouseDevice mouse;
		KeyboardDevice keyboard;
	private:
		Event<void(KEYBOARD_BUTTONS keycode, KEY_ACTION action)> onKeyAction;

		std::vector<std::weak_ptr<input_manager_base>> input_managers_list;
		std::queue<inp::input_event> event_queque;
	};

	input_system& get_system();
}
 