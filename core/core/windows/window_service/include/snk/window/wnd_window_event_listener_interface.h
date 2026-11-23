#pragma once
#include <cstdint>
#include <snk/core_input/inp_input_event.hpp>

namespace wnd
{
	using handle = void*;

	// A module fills this vtable and the engine calls it: raised with any change of its bases or virtual functions
	// (check_vtable_pins.py).
	inline constexpr std::uint32_t window_listener_interface_abi_revision = 1;

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