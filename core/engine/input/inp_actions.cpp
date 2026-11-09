#include <snk/input/inp_actions.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>

bool inp::action_map::add(std::string name, std::vector<action_binding> bindings)
{
	if (name.empty() || std::ranges::any_of(m_actions, [&](const action& a) { return a.name == name; })) return false;
	m_actions.push_back(action{ std::move(name), std::move(bindings) });
	return true;
}

inp::action_id inp::action_map::id(std::string_view name) const
{
	const auto it = std::ranges::find_if(m_actions, [&](const action& a) { return a.name == name; });
	if (it != m_actions.end()) return static_cast<action_id>(std::distance(m_actions.begin(), it));
	if (m_warned.emplace(name).second) {
		egLOG_WARN("input", "no input action '{}' in res/input/actions.desc", name);
	}
	return invalid_action;
}

const inp::action* inp::action_map::find(action_id id) const
{
	return id < m_actions.size() ? &m_actions[id] : nullptr;
}
