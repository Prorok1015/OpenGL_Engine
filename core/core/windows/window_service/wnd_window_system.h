#pragma once
#include "wnd_window.h"
#include "wnd_window_event_listener_interface.h"
#include "rnd_render_contex_interface.h"
#include <algorithm>
#include <memory>
#include <vector>
#include "snk_api.h"

namespace wnd
{
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
		virtual rnd::driver::render_context_interface* get_context() const = 0;
		// Native window handle + DPI scale, exposed so the GUI layer can set up its imgui backend
		// without window depending on gui (EPIC-49 US-49-9). window no longer owns the gui backend.
		virtual void* get_active_native_handle() const = 0;
		virtual float get_active_content_scale() const = 0;
		virtual bool is_stop_running() = 0;

		virtual void pool_events() const = 0;
		virtual void init_windows_frame_time() const = 0;
		virtual void process_windows() const = 0;

		void add_event_listener(std::shared_ptr<window_listener_interface> listener) {
			m_event_listeners.push_back(listener);
		}

		void remove_event_listener(std::shared_ptr<window_listener_interface> listener) {
			m_event_listeners.erase(std::remove(m_event_listeners.begin(), m_event_listeners.end(), listener), m_event_listeners.end());
		}

		const std::vector<std::shared_ptr<window_listener_interface>>& get_event_listeners() const {
			return m_event_listeners;
		}

	private:
		std::vector<std::shared_ptr<window_listener_interface>> m_event_listeners;
	};
}
