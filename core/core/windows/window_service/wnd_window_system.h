#pragma once
#include "wnd_window.h"
#include "wnd_window_event_listener_interface.h"
#include "rnd_render_contex_interface.h"
#include "gui_backend_interface.h" // TODO: construct in app data storage
#include <string>

namespace wnd
{
	class window_system
	{
	public:
		window_system();
		~window_system();
		window_system(window_system&&) = delete;
		window_system& operator= (window_system&&) = delete;
		window_system(const window_system&) = delete;
		window_system& operator= (const window_system&) = delete;

		void init();

		std::shared_ptr<window> get_active_window();
		std::shared_ptr<window> find_window(window::short_id win);
		rnd::driver::render_context_interface* get_context() const { return context.get(); }
		gui::imgui_backend_interface* get_gui_backend() const { return imgui_backend.get(); }
		bool is_stop_running();

		void pool_events() const;
		void init_windows_frame_time() const;
		void process_windows() const;

		void add_event_listener(std::shared_ptr<window_listener_interface> listener) {
			event_listeners.push_back(listener);
		}

		void remove_event_listener(std::shared_ptr<window_listener_interface> listener) {
			event_listeners.erase(std::remove(event_listeners.begin(), event_listeners.end(), listener), event_listeners.end());
		}

		const std::vector<std::shared_ptr<window_listener_interface>>& get_event_listeners() const {
			return event_listeners;
		}

	private:
		std::shared_ptr<window> make_window(); 

	private:
		std::vector<std::shared_ptr<window_listener_interface>> event_listeners;
		std::unordered_map<window::short_id, std::shared_ptr<window>, window::short_id::hasher> windows_list;
		std::unique_ptr<rnd::driver::render_context_interface> context = nullptr;
		std::unique_ptr<gui::imgui_backend_interface> imgui_backend = nullptr;

		window::short_id active_window;
		wnd::context::header title;
	};
}
