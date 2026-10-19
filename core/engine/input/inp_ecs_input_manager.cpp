#include <snk/input/inp_ecs_input_manager.h>
#include <snk/core_input/inp_keyboard_event.hpp>
#include <snk/input/inp_events.hpp>
#include <snk/ecs/ecs_system.h>

void inp::register_ecs_input_system(ecs::system_factory& factory, std::shared_ptr<ecs_input_manager> manager)
{
	factory.register_system<&ecs_input_manager::update_input_state_system>("inp::update_input_state",
		{ .phase = ecs::phase::input, .run_in_edit = true }, std::move(manager));
}

void inp::ecs_input_manager::on_focus_lost(wnd::handle)
{
	m_raw.keyboard.fill(false);
	m_raw.mouse.fill(false);
}

bool inp::ecs_input_manager::on_handle_event(wnd::handle win, const inp::input_event& evt)
{
	if (const auto* payload = evt.get_payload<core::inp::keyboard_event>()) {
		m_raw.keyboard[(std::underlying_type_t<inp::KEYBOARD_BUTTONS>)payload->key] = payload->action == core::inp::KEY_ACTION::DOWN;
	} else if (const auto* payload = evt.get_payload<core::inp::mouse_click_event>()) {
		m_raw.mouse[(std::underlying_type_t<inp::MOUSE_BUTTONS>)payload->key] = payload->action == core::inp::KEY_ACTION::DOWN;
	} else if (const auto* payload = evt.get_payload<core::inp::cursor_move_event>()) {
		m_raw.mouse_prev = m_raw.mouse_pos;
		m_raw.mouse_pos = payload->pos;
		m_raw.mouse_dir = m_raw.mouse_prev - m_raw.mouse_pos;
	} else if (const auto* payload = evt.get_payload<core::inp::scroll_move_event>()) {
		m_raw.scroll = payload->direction;
	}
	return false;
}
