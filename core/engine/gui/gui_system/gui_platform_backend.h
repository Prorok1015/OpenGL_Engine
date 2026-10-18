#pragma once
#include <snk/window/wnd_window.h>
#include <snk/window/wnd_window_event_listener_interface.h>
#include "snk_api.h"
#include <chrono>
#include <cstdint>
#include <string>

struct ImGuiContext;

namespace gui
{
	// Platform half of the ImGui integration: feeds ImGui from wnd events and window state, with no
	// GLFW dependency. Register it as a wnd::window_system listener; it never consumes anything, so
	// who gets an input event (ImGui vs engine layers) is still decided by WantCapture*.
	// ImGui runs in window units with DisplayFramebufferScale; wnd cursor events are framebuffer
	// pixels, converted back in one place (on_input_event).
	class SNK_API platform_backend final : public wnd::window_listener_interface
	{
	public:
		explicit platform_backend(wnd::window& window);
		~platform_backend() override;

		platform_backend(const platform_backend&) = delete;
		platform_backend& operator= (const platform_backend&) = delete;

		// The ImGui context must be current and outlive shutdown().
		void init();
		void shutdown();

		// Display size / scale / delta time / mouse cursor. Call before ImGui::NewFrame().
		void new_frame();

		void on_input_event(wnd::handle win, const inp::input_event& evt) override;
		void on_window_focus_gained(wnd::handle win) override;
		void on_window_focus_lost(wnd::handle win) override;
		void on_window_resize(wnd::handle, int, int) override {}
		void on_window_moved(wnd::handle, int, int) override {}
		void on_window_close(wnd::handle) override {}
		void on_window_refresh(wnd::handle) override {}
		void on_window_minimized(wnd::handle) override {}
		void on_window_restored(wnd::handle) override {}
		void on_window_maximized(wnd::handle) override {}
		void on_window_created(wnd::handle) override {}
		void on_window_destroyed(wnd::handle) override {}

	private:
		static const char* get_clipboard_text(ImGuiContext* ctx);
		static void set_clipboard_text(ImGuiContext* ctx, const char* text);

		void update_mouse_cursor();
		void update_modifiers(std::uint8_t previous);

		wnd::window& m_window;
		std::chrono::steady_clock::time_point m_last_frame{};
		bool m_has_last_frame = false;
		// Bit per physical modifier key, so releasing one of two held Shifts keeps Shift down.
		std::uint8_t m_modifier_keys = 0;
		bool m_cursor_hidden = false;
		bool m_cursor_shape_valid = false;
		wnd::cursor_shape m_cursor_shape = wnd::cursor_shape::arrow;
		// ImGui keeps the pointer returned by the clipboard getter until the next call.
		std::string m_clipboard;
		bool m_initialized = false;
	};
}
