#pragma once
#include "inp_input_event.hpp"

namespace wnd
{
	using handle = void*;
	class window_listener_interface
	{
	public:
		virtual ~window_listener_interface() = default;
		virtual void on_input_event(wnd::handle win, const inp::input_event& evt) = 0;
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