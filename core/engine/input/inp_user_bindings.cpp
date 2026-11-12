#include <snk/input/inp_user_bindings.h>
#include <algorithm>

namespace
{
	const std::vector<inp::action_binding>& current_bindings(const inp::user_bindings& user, const inp::action& project_action)
	{
		const auto it = user.overrides().find(project_action.name);
		return it != user.overrides().end() ? it->second : project_action.bindings;
	}
}

inp::rebind_result inp::user_bindings::rebind(const action_map& project, std::string_view action, std::size_t slot, action_binding binding)
{
	const auto actions = project.actions();
	const auto target = std::ranges::find_if(actions, [&](const inp::action& a) { return a.name == action; });
	if (target == actions.end()) return { rebind_status::unknown_action, {} };

	std::vector<action_binding> bindings = current_bindings(*this, *target);
	if (slot > bindings.size()) return { rebind_status::bad_slot, {} };
	for (std::size_t i = 0; i < bindings.size(); ++i) {
		if (i != slot && bindings[i] == binding) return { rebind_status::duplicate, {} };
	}

	for (const inp::action& other : actions) {
		if (other.name == action) continue;
		const auto& theirs = current_bindings(*this, other);
		if (std::ranges::find(theirs, binding) != theirs.end()) return { rebind_status::conflict, other.name };
	}

	if (slot == bindings.size()) {
		bindings.push_back(binding);
	} else {
		bindings[slot] = binding;
	}
	m_overrides.insert_or_assign(target->name, std::move(bindings));
	return {};
}

bool inp::user_bindings::reset(std::string_view action)
{
	const auto it = m_overrides.find(action);
	if (it == m_overrides.end()) return false;
	m_overrides.erase(it);
	return true;
}

void inp::user_bindings::set(std::string action, std::vector<action_binding> bindings)
{
	m_overrides.insert_or_assign(std::move(action), std::move(bindings));
}

inp::action_map inp::user_bindings::apply(const action_map& project) const
{
	action_map effective;
	for (const inp::action& a : project.actions()) {
		effective.add(a.name, current_bindings(*this, a));
	}
	return effective;
}

std::vector<inp::binding_conflict> inp::find_binding_conflicts(const action_map& map)
{
	std::vector<binding_conflict> found;
	const auto actions = map.actions();
	for (std::size_t i = 0; i < actions.size(); ++i) {
		for (std::size_t j = i + 1; j < actions.size(); ++j) {
			for (const action_binding& binding : actions[i].bindings) {
				if (std::ranges::find(actions[j].bindings, binding) != actions[j].bindings.end()) {
					found.push_back({ binding, actions[i].name, actions[j].name });
				}
			}
		}
	}
	return found;
}
