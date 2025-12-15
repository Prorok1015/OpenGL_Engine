#include "inp_input_system.h"
#include <engine_log.h>

void inp::input_system::on_input_event(wnd::handle win, const wnd::input_event& event)
{
	for (auto& listener : m_consumable_listeners) {
			if (listener->on_handle_event(win, event)) {
			return;
		}
	}

	wnd::handle target_win = win;

	if (!target_win) {
		target_win = get_focused_window();
		if (target_win == 0) return;
	}
	
	auto it_stack = m_window_layers.find(target_win);
	if (it_stack == m_window_layers.end()) return;

	auto& stack = it_stack->second;
	
	for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
		input_layer& layer = *it;

		const bool handled = layer.processor->on_handle_event(target_win, event);

		if (handled && layer.blocks_lower_layers) {
			break;
		}
	}
}