#pragma once
#include "common.h"
#include "ds_type_id.hpp"
#include "inp_keyboard_device.h"
#include "inp_mouse_device.h"
#include "inp_input_manager_base.h"
#include "wnd_window_event_listener_interface.h"
#include "inp_input_consumable_listener.h"
#include <unordered_map>

namespace inp
{
	struct input_layer
	{
		std::shared_ptr<input_manager_base> processor;
		bool blocks_lower_layers = false;
	};

	class input_system : public wnd::window_listener_interface
	{
	public:
		input_system();
		~input_system();
		input_system(input_system&&) = delete;
		input_system& operator= (input_system&&) = delete;
		input_system(const input_system&) = delete;
		input_system& operator= (const input_system&) = delete;
		

		void push_input_layer(wnd::handle win, const input_layer& layer) { m_window_layers[win].push_back(layer); }
		void pop_input_layer(wnd::handle win, std::shared_ptr<input_manager_base> processor) {
			auto& layers = m_window_layers[win];
			layers.erase(std::remove_if(layers.begin(), layers.end(),
				[processor](const input_layer& layer) {
					return layer.processor == processor;
				}), layers.end());
		}

		void push_consumable_listener(std::shared_ptr<input_event_consumable_listener_interface> listener) {
			m_consumable_listeners.push_back(listener);
		}
		void pop_consumable_listener(std::shared_ptr<input_event_consumable_listener_interface> listener) {
			m_consumable_listeners.erase(std::remove_if(m_consumable_listeners.begin(), m_consumable_listeners.end(),
				[listener](const std::shared_ptr<input_event_consumable_listener_interface>& ptr) {
					return ptr.get() == listener.get();
				}), m_consumable_listeners.end());
		}

		virtual void on_input_event(wnd::handle win, const wnd::input_event& evt) override;

		// TODO: delete these methods and use only on_input_event
		void process_input(float dt);
		void activate_manager(std::weak_ptr<input_manager_base> inp_manager);
		void deactivate_manager(std::weak_ptr<input_manager_base> inp_manager);
		Key get_key_state(KEYBOARD_BUTTONS) const;
		Key get_key_state(MOUSE_BUTTONS) const;
		void on_keyboard_event(const keyboard_event&);
		void on_mouse_buttons_event(const mouse_click_event&);
		void on_cursor_move_event(const cursor_move_event&);
		void on_scroll_move_event(const scroll_move_event&);		
		virtual void on_key_input(wnd::handle win, wnd::KEYBOARD_BUTTONS key, int scancode, wnd::KEY_ACTION action, int mods) override;
		virtual void on_char_input(wnd::handle win, wchar_t codepoint) override;
		virtual void on_mouse_button_input(wnd::handle win, wnd::MOUSE_BUTTONS button, wnd::KEY_ACTION action, int mods) override;
		virtual void on_mouse_moved(wnd::handle win, double xpos, double ypos) override;
		virtual void on_mouse_scrolled(wnd::handle win, double xoffset, double yoffset) override;
		//

		virtual void on_window_focus_gained(wnd::handle win)  override { m_focused_window = win; }
		virtual void on_window_focus_lost(wnd::handle win)  override { m_focused_window = nullptr; }
		virtual void on_window_resize(wnd::handle win, int width, int height) override;
		virtual void on_window_moved(wnd::handle win, int xpos, int ypos)  override {};
		virtual void on_window_close(wnd::handle win)  override {};
		virtual void on_window_refresh(wnd::handle win)  override {};
		virtual void on_window_minimized(wnd::handle win)  override {};
		virtual void on_window_restored(wnd::handle win)  override {};
		virtual void on_window_maximized(wnd::handle win)  override {};
		virtual void on_window_created(wnd::handle win)  override { if (!m_focused_window) m_focused_window = win; };
		virtual void on_window_destroyed(wnd::handle win)  override { if (win == m_focused_window) m_focused_window = {}; };

		wnd::handle get_focused_window() const { return m_focused_window; }
	private:

	public:
		MouseDevice mouse;
		KeyboardDevice keyboard;
	private:
		Event<void(KEYBOARD_BUTTONS keycode, KEY_ACTION action)> onKeyAction;
		std::vector<std::weak_ptr<input_manager_base>> input_managers_list;
		std::queue<inp::input_event> event_queque;


		wnd::handle m_focused_window = {};
		std::unordered_map<wnd::handle, std::vector<input_layer>> m_window_layers;
		std::vector<std::shared_ptr<input_event_consumable_listener_interface>> m_consumable_listeners;
	};

	input_system& get_system();
}
 