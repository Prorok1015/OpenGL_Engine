#pragma once
#include <common.h>
#include <ds_type_id.hpp>
#include "inp_keyboard_device.h"
#include "inp_mouse_device.h"
#include "inp_input_manager_base.h"
#include "wnd_window_event_listener_interface.h"

namespace inp
{
	class input_system : public wnd::window_listener_interface
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

		virtual void on_key_input(wnd::handle win, wnd::KEYBOARD_BUTTONS key, int scancode, wnd::KEY_ACTION action, int mods) override;
		virtual void on_char_input(wnd::handle win, wchar_t codepoint) override;
		virtual void on_mouse_button_input(wnd::handle win, wnd::MOUSE_BUTTONS button, wnd::KEY_ACTION action, int mods) override;
		virtual void on_mouse_moved(wnd::handle win, double xpos, double ypos) override;
		virtual void on_mouse_scrolled(wnd::handle win, double xoffset, double yoffset) override;
		virtual void on_window_focus_gained(wnd::handle win)  override {};
		virtual void on_window_focus_lost(wnd::handle win)  override {};
		virtual void on_window_resize(wnd::handle win, int width, int height) override;
		virtual void on_window_moved(wnd::handle win, int xpos, int ypos)  override {};
		virtual void on_window_close(wnd::handle win)  override {};
		virtual void on_window_refresh(wnd::handle win)  override {};
		virtual void on_window_minimized(wnd::handle win)  override {};
		virtual void on_window_restored(wnd::handle win)  override {};
		virtual void on_window_maximized(wnd::handle win)  override {};
		virtual void on_window_created(wnd::handle win)  override {};
		virtual void on_window_destroyed(wnd::handle win)  override {};

		MouseDevice mouse;
		KeyboardDevice keyboard;
	private:
		Event<void(KEYBOARD_BUTTONS keycode, KEY_ACTION action)> onKeyAction;

		std::vector<std::weak_ptr<input_manager_base>> input_managers_list;
		std::queue<inp::input_event> event_queque;
	};

	input_system& get_system();
}
 