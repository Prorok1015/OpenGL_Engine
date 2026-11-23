#pragma once
#include <snk/window/wnd_window.h>
#include <snk/window/wnd_window_event_listener_interface.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <snk/common/snk_api.h>

namespace wnd
{
	// Module code calls this vtable: raised with any change of its bases or virtual functions (check_vtable_pins.py).
	inline constexpr std::uint32_t window_system_abi_revision = 1;

	// Backend-agnostic window system: concrete implementations live behind this interface.
	class SNK_API window_system
	{
	public:
		window_system();
		virtual ~window_system();
		window_system(window_system&&) = delete;
		window_system& operator= (window_system&&) = delete;
		window_system(const window_system&) = delete;
		window_system& operator= (const window_system&) = delete;

		virtual void init() = 0;

		virtual std::shared_ptr<window> get_active_window() = 0;
		virtual float get_active_content_scale() const = 0;
		virtual bool is_stop_running() = 0;

		virtual void pool_events() const = 0;
		virtual void init_windows_frame_time() const = 0;
		virtual void process_windows() const = 0;

		// Exported, not inline: module code calling them must not compile the layout of the members (#1256).
		void add_event_listener(std::shared_ptr<window_listener_interface> listener);
		void remove_event_listener(std::shared_ptr<window_listener_interface> listener);
		const std::vector<std::shared_ptr<window_listener_interface>>& get_event_listeners() const;

		// Called for a close request from the user (X button, Alt+F4) instead of closing at once.
		// The handler decides: window::shutdown() closes, doing nothing keeps the window open.
		using close_request_handler = std::function<void()>;
		void set_close_request_handler(close_request_handler handler);

		// Backends call this when the user asks to close `win`. The window stays open unless the handler
		// shuts it down; without a handler it closes at once.
		void request_close(window& win);

	private:
		std::vector<std::shared_ptr<window_listener_interface>> m_event_listeners;
		close_request_handler m_close_handler;
	};
}
