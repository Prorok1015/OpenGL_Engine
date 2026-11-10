#include <snk/input/inp_action_state.h>
#include <snk/common/logger/engine_log.h>
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

	// Some binding of the action reaches a layer of the world that declares it (whether it is down or not).
	bool reachable(const inp::action& action, const inp::action_reach& reach)
	{
		for (const auto& binding : action.bindings) {
			const bool reached = std::visit([&reach](auto button) {
				if constexpr (std::is_same_v<decltype(button), inp::KEYBOARD_BUTTONS>) {
					return reach.keys.test(static_cast<std::size_t>(button));
				} else {
					return reach.buttons;
				}
			}, binding);
			if (reached) return true;
		}
		return false;
	}

	// A binding counts when it is down and reached a layer of the world that declares the action.
	bool reached_down(const inp::action& action, const inp::input_state& state, const inp::action_reach& reach)
	{
		for (const auto& binding : action.bindings) {
			const bool reached = std::visit([&reach](auto button) {
				if constexpr (std::is_same_v<decltype(button), inp::KEYBOARD_BUTTONS>) {
					return reach.keys.test(static_cast<std::size_t>(button));
				} else {
					return reach.buttons;
				}
			}, binding);
			if (reached && binding_down(binding, state)) return true;
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
	ecs::event_writer<action_event>* events, fixed_action_state* fixed, const action_input* reached)
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

		bool down = false;
		// An action that reaches no layer of the world cannot be re-armed: when it reaches one again with its key still
		// held (a modal layer closed), the press waits for a release.
		bool arms = true;
		if (bound.bound) {
			if (reached && !reached->reach.empty()) {
				const bool in_range = id < reached->reach.size();
				down = in_range && reached_down(list[id], state, reached->reach[id]);
				arms = in_range && reachable(list[id], reached->reach[id]);
			} else {
				down = action_down(list[id], state);
			}
		}
		if (fresh) {
			s.armed = bound.bound && arms && !down;
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
		if (!bound.bound || !arms) {
			s.armed = false;
		} else if (!down) {
			s.armed = true;
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
	const input_bound& bound, const action_map_ref& actions, const action_input& reached, action_state& out, fixed_action_state& fixed)
{
	out.update(actions, state, bound, &events, &fixed, &reached);
}
