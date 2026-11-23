#include <snk/window/wnd_window.h>
#include <snk/window/wnd_window_system.h>
#include <utility>

wnd::window::~window() = default;

wnd::window_system::window_system() = default;
wnd::window_system::~window_system() = default;

void wnd::window_system::add_event_listener(std::shared_ptr<window_listener_interface> listener)
{
	m_event_listeners.push_back(std::move(listener));
}

void wnd::window_system::remove_event_listener(std::shared_ptr<window_listener_interface> listener)
{
	std::erase(m_event_listeners, listener);
}

const std::vector<std::shared_ptr<wnd::window_listener_interface>>& wnd::window_system::get_event_listeners() const
{
	return m_event_listeners;
}

void wnd::window_system::set_close_request_handler(close_request_handler handler)
{
	m_close_handler = std::move(handler);
}

void wnd::window_system::request_close(window& win)
{
	if (!m_close_handler) {
		win.shutdown();
		return;
	}
	win.shutdown(false);
	m_close_handler();
}
