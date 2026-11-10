#include <snk/input/inp_action_state.h>
#include <snk/common/logger/engine_log.h>
#include <cstdint>
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

	bool binding_reached(const inp::action_binding& binding, const inp::action_reach& reach)
	{
		return std::visit([&reach](auto button) {
			if constexpr (std::is_same_v<decltype(button), inp::KEYBOARD_BUTTONS>) {
				return reach.keys.test(static_cast<std::size_t>(button));
			} else {
				return reach.buttons;
			}
		}, binding);
	}

	bool binding_armed(const inp::action_binding& binding, const inp::action_reach& reach)
	{
		return std::visit([&reach](auto button) {
			const auto index = static_cast<std::size_t>(button);
			if constexpr (std::is_same_v<decltype(button), inp::KEYBOARD_BUTTONS>) {
				return reach.armed_keys.test(index);
			} else {
				return index < inp::mouse_count && reach.armed_buttons.test(index);
			}
		}, binding);
	}

	struct binding_view
	{
		// Some binding is down and reached a layer of the world that declares the action.
		bool down = false;
		// ...and is armed there (the memory of fronts) and was not down when the map came in.
		bool may_press = false;
		// The bindings (by index) that are down, reached or not.
		std::uint64_t down_bits = 0;
	};

	binding_view evaluate(const inp::action& action, const inp::input_state& state, const inp::action_reach& reach, std::uint64_t suppressed)
	{
		binding_view view;
		for (std::size_t i = 0; i < action.bindings.size(); ++i) {
			const auto& binding = action.bindings[i];
			if (!binding_down(binding, state)) continue;
			if (i < 64) view.down_bits |= std::uint64_t{ 1 } << i;
			if (!binding_reached(binding, reach)) continue;
			view.down = true;
			const bool fresh_binding = i >= 64 || (suppressed & (std::uint64_t{ 1 } << i)) == 0;
			if (fresh_binding && binding_armed(binding, reach)) view.may_press = true;
		}
		return view;
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
	ecs::event_writer<action_event>* events, fixed_action_state* fixed, const action_input* reached, const action_injection* injected)
{
	const auto emit = [events, fixed](action_id id, action_edge edge) {
		if (events && events->bound()) events->emit(action_event{ id, edge });
		if (fixed) fixed->push(id, edge);
	};

	const bool fresh = actions.map != m_map;
	if (fresh) {
		// A subscriber that saw the press of the old map must also see its release. The fixed steps get theirs from
		// set_map below, after what they were last shown.
		for (action_id id = 0; id < m_slots.size(); ++id) {
			if (m_slots[id].held && events && events->bound()) events->emit(action_event{ id, action_edge::released });
		}
		m_map = actions.map;
		m_slots.assign(m_map ? m_map->size() : 0, slot{});
	}
	if (fixed && fixed->map() != m_map) fixed->set_map(m_map);
	if (!m_map) return;

	const auto list = m_map->actions();
	for (action_id id = 0; id < m_slots.size(); ++id) {
		slot& s = m_slots[id];
		s.pressed = false;
		s.released = false;
		if (!bound.bound && bound.frozen) continue;

		const bool with_reach = reached && !reached->reach.empty();
		bool down = false;
		bool may_press = true;
		std::uint64_t down_bits = 0;
		if (bound.bound) {
			if (with_reach) {
				const action_reach none{};
				const action_reach& reach = id < reached->reach.size() ? reached->reach[id] : none;
				const binding_view view = evaluate(list[id], state, reach, s.suppressed);
				down = view.down;
				may_press = view.may_press;
				down_bits = view.down_bits;
			} else {
				down = action_down(list[id], state);
			}
		}
		if (fresh) {
			s.suppressed = down_bits;
			s.armed = bound.bound && !down;
		} else {
			if (s.held && !down && !s.injected) {
				s.held = false;
				s.released = true;
				emit(id, action_edge::released);
			} else if (!s.held && down && may_press && (with_reach || s.armed)) {
				s.held = true;
				s.pressed = true;
				emit(id, action_edge::pressed);
			}
			if (!bound.bound) {
				s.armed = false;
			} else if (!down) {
				s.armed = true;
			}
			s.suppressed &= down_bits;
		}
		if (injected) {
			for (const injected_edge& edge : injected->edges) {
				if (edge.action != id) continue;
				if (edge.edge == action_edge::pressed) {
					s.injected = true;
					if (s.held) continue;
					s.held = true;
					s.pressed = true;
					emit(id, action_edge::pressed);
				} else {
					s.injected = false;
					if (!s.held || down) continue;
					s.held = false;
					s.released = true;
					emit(id, action_edge::released);
				}
			}
		}
		if (fixed) fixed->set_held(id, s.held);
	}
}

const inp::fixed_action_state::slot* inp::fixed_action_state::at(action_id id) const
{
	return id < m_slots.size() ? &m_slots[id] : nullptr;
}

inp::action_id inp::fixed_action_state::find(std::string_view name) const
{
	return m_map ? m_map->id(name) : invalid_action;
}

bool inp::fixed_action_state::held(action_id id) const
{
	const slot* s = at(id);
	return s && s->held;
}

bool inp::fixed_action_state::pressed(action_id id) const
{
	const slot* s = at(id);
	return s && s->has_step_edge && s->step_edge == action_edge::pressed;
}

bool inp::fixed_action_state::released(action_id id) const
{
	const slot* s = at(id);
	return s && s->has_step_edge && s->step_edge == action_edge::released;
}

bool inp::fixed_action_state::held(std::string_view name) const
{
	return held(find(name));
}

bool inp::fixed_action_state::pressed(std::string_view name) const
{
	return pressed(find(name));
}

bool inp::fixed_action_state::released(std::string_view name) const
{
	return released(find(name));
}

void inp::fixed_action_state::begin_step()
{
	for (slot& s : m_slots) {
		s.has_step_edge = !s.queue.empty();
		if (!s.has_step_edge) continue;
		s.step_edge = s.queue.front();
		s.queue.pop_front();
		s.shown_down = s.step_edge == action_edge::pressed;
	}
}

void inp::fixed_action_state::set_map(std::shared_ptr<const action_map> map)
{
	std::vector<slot> fresh(map ? map->size() : 0);
	for (action_id id = 0; id < m_slots.size(); ++id) {
		if (!m_slots[id].shown_down) continue;
		// The old map may be longer: the slot exists only to hand that released over.
		if (id >= fresh.size()) fresh.resize(static_cast<std::size_t>(id) + 1);
		fresh[id].queue.push_back(action_edge::released);
	}
	m_map = std::move(map);
	m_slots = std::move(fresh);
}

void inp::fixed_action_state::push(action_id id, action_edge edge)
{
	if (id >= m_slots.size()) return;
	slot& s = m_slots[id];
	if (s.queue.size() >= queue_limit) {
		s.queue.pop_front();
		s.queue.pop_front();
		if (!s.warned) {
			s.warned = true;
			egLOG_WARN("input", "action {}: more than {} edges waited for a fixed step, the oldest pairs are dropped",
				m_map && m_map->find(id) ? m_map->find(id)->name : std::string{ "?" }, queue_limit);
		}
	}
	s.queue.push_back(edge);
}

void inp::fixed_action_state::set_held(action_id id, bool held)
{
	if (id < m_slots.size()) m_slots[id].held = held;
}

void inp::update_actions_system(ecs::event_writer<action_event>& events, const input_state& state,
	const input_bound& bound, const action_map_ref& actions, const action_input& reached, const action_injection& injected,
	action_state& out, fixed_action_state& fixed)
{
	out.update(actions, state, bound, &events, &fixed, &reached, &injected);
}
