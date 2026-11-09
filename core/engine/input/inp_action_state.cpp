#include <snk/input/inp_action_state.h>
#include <type_traits>
#include <variant>

namespace
{
	bool binding_down(const inp::action_binding& binding, const inp::input_state& state)
	{
		return std::visit([&state](auto button) {
			const auto index = static_cast<std::size_t>(button);
			if constexpr (std::is_same_v<decltype(button), inp::KEYBOARD_BUTTONS>) {
				return index < state.keyboard.size() && state.keyboard[index];
			} else {
				return index < state.mouse.size() && state.mouse[index];
			}
		}, binding);
	}

	bool action_down(const inp::action& action, const inp::input_state& state)
	{
		for (const auto& binding : action.bindings) {
			if (binding_down(binding, state)) return true;
		}
		return false;
	}
}

const inp::action_state::slot* inp::action_state::at(action_id id) const
{
	return id < m_slots.size() ? &m_slots[id] : nullptr;
}

inp::action_id inp::action_state::find(std::string_view name) const
{
	return m_map ? m_map->id(name) : invalid_action;
}

bool inp::action_state::held(action_id id) const
{
	const slot* s = at(id);
	return s && s->held;
}

bool inp::action_state::pressed(action_id id) const
{
	const slot* s = at(id);
	return s && s->pressed;
}

bool inp::action_state::released(action_id id) const
{
	const slot* s = at(id);
	return s && s->released;
}

bool inp::action_state::held(std::string_view name) const
{
	return held(find(name));
}

bool inp::action_state::pressed(std::string_view name) const
{
	return pressed(find(name));
}

bool inp::action_state::released(std::string_view name) const
{
	return released(find(name));
}

void inp::action_state::update(const action_map_ref& actions, const input_state& state, const input_bound& bound,
	ecs::event_writer<action_event>* events)
{
	const auto emit = [events](action_id id, action_edge edge) {
		if (events && events->bound()) events->emit(action_event{ id, edge });
	};

	const bool fresh = actions.map != m_map;
	if (fresh) {
		// A subscriber that saw the press of the old map must also see its release.
		for (action_id id = 0; id < m_slots.size(); ++id) {
			if (m_slots[id].held) emit(id, action_edge::released);
		}
		m_map = actions.map;
		m_slots.assign(m_map ? m_map->size() : 0, slot{});
	}
	if (!m_map) return;

	const auto list = m_map->actions();
	for (action_id id = 0; id < m_slots.size(); ++id) {
		slot& s = m_slots[id];
		s.pressed = false;
		s.released = false;
		if (!bound.bound && bound.frozen) continue;

		const bool down = bound.bound && action_down(list[id], state);
		if (fresh) {
			s.armed = bound.bound && !down;
			continue;
		}
		if (s.held && !down) {
			s.held = false;
			s.released = true;
			emit(id, action_edge::released);
		} else if (!s.held && down && s.armed) {
			s.held = true;
			s.pressed = true;
			emit(id, action_edge::pressed);
		}
		if (!bound.bound) {
			s.armed = false;
		} else if (!down) {
			s.armed = true;
		}
	}
}

void inp::update_actions_system(ecs::event_writer<action_event>& events, const input_state& state,
	const input_bound& bound, const action_map_ref& actions, action_state& out)
{
	out.update(actions, state, bound, &events);
}
