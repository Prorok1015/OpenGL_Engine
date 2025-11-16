#pragma once
#include "wnd_input_enums.hpp"

namespace wnd
{
	using handle = void*;
	class window_listener_interface
	{
	public:
		virtual ~window_listener_interface() = default;
		virtual void on_key_input(wnd::handle win, wnd::KEYBOARD_BUTTONS key, int scancode, wnd::KEY_ACTION action,int mods) = 0;
		virtual void on_char_input(wnd::handle win, wchar_t codepoint) = 0;
		virtual void on_mouse_button_input(wnd::handle win, wnd::MOUSE_BUTTONS button, wnd::KEY_ACTION action, int mods) = 0;
		virtual void on_mouse_moved(wnd::handle win, double xpos, double ypos) = 0;
		virtual void on_mouse_scrolled(wnd::handle win, double xoffset, double yoffset) = 0;
		virtual void on_window_focus_gained(wnd::handle win) = 0;
		virtual void on_window_focus_lost(wnd::handle win) = 0;
		virtual void on_window_resize(wnd::handle win, int width, int height) = 0;
		virtual void on_window_moved(wnd::handle win, int xpos, int ypos) = 0;
		virtual void on_window_close(wnd::handle win) = 0;
		virtual void on_window_refresh(wnd::handle win) = 0;
		virtual void on_window_minimized(wnd::handle win) = 0;
		virtual void on_window_restored(wnd::handle win) = 0;
		virtual void on_window_maximized(wnd::handle win) = 0;
		virtual void on_window_created(wnd::handle win) = 0;
		virtual void on_window_destroyed(wnd::handle win) = 0;
	};
}