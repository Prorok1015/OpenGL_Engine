#include "edt_input_actions_view.h"
#include <snk/input/inp_input_stack.h>
#include <algorithm>
#include <format>
#include <type_traits>
#include <variant>

std::string edt::input_actions_view::binding_text(const inp::action_binding& binding)
{
	if (const auto key = binding.key()) return std::string{ inp::key_code_name(*key) };
	if (const auto button = binding.mouse_button()) return "Mouse " + std::string{ inp::mouse_button_name(*button) };
	if (const auto pad = binding.gamepad_button()) return "Pad " + std::string{ inp::gamepad_button_name(*pad) };
	if (const auto axis = binding.gamepad_axis()) return "Axis " + std::string{ inp::gamepad_axis_name(*axis) };
	return {};
}

std::vector<edt::input_actions_view::action_row> edt::input_actions_view::build_rows(const inp::action_map& project,
	const inp::action_map& effective, const std::vector<inp::binding_conflict>& conflicts)
{
	std::vector<action_row> rows;
	rows.reserve(effective.size());
	for (const inp::action& action : effective.actions()) {
		action_row row;
		row.name = action.name;
		const inp::action* original = project.find(project.id(action.name));
		row.bindings.reserve(action.bindings.size());
		for (std::size_t slot = 0; slot < action.bindings.size(); ++slot) {
			binding_row binding;
			binding.text = binding_text(action.bindings[slot]);
			binding.changed = !original || slot >= original->bindings.size() || original->bindings[slot] != action.bindings[slot];
			row.changed = row.changed || binding.changed;
			row.bindings.push_back(std::move(binding));
		}
		row.changed = row.changed || (original && original->bindings.size() != action.bindings.size());
		for (const inp::binding_conflict& conflict : conflicts) {
			const std::string* other = conflict.first == action.name ? &conflict.second : conflict.second == action.name ? &conflict.first : nullptr;
			if (other && std::ranges::find(row.shares_with, *other) == row.shares_with.end()) row.shares_with.push_back(*other);
		}
		rows.push_back(std::move(row));
	}
	return rows;
}

std::vector<edt::problem> edt::input_actions_view::conflict_problems(const std::vector<inp::binding_conflict>& conflicts)
{
	std::vector<problem> problems;
	problems.reserve(conflicts.size());
	for (const inp::binding_conflict& conflict : conflicts) {
		problem item;
		item.severity = problem_severity::warning;
		item.message = std::format("Input actions '{}' and '{}' share {}", conflict.first, conflict.second, binding_text(conflict.binding));
		problems.push_back(std::move(item));
	}
	return problems;
}

void edt::input_actions_view::capture::begin(std::string action, std::size_t slot)
{
	m_active = true;
	m_action = std::move(action);
	m_slot = slot;
}

std::string edt::input_actions_view::outcome_text(const gs::rebind_outcome& outcome, std::string_view action,
	const inp::action_binding* binding)
{
	const std::string key = binding ? binding_text(*binding) : std::string{};
	switch (outcome.result.status) {
	case inp::rebind_status::applied: {
		std::string text;
		if (outcome.result.conflicted()) {
			text = std::format("{} is also used by '{}': both keep it", key, outcome.result.conflict_with);
		}
		if (!outcome.saved) {
			text += (text.empty() ? "" : ". ") + std::string{ "Not saved: the change holds for this run only" };
		}
		return text;
	}
	case inp::rebind_status::duplicate:
		return std::format("'{}' already has {} in another slot", action, key);
	case inp::rebind_status::bad_slot:
		return "That slot is not there any more";
	case inp::rebind_status::unknown_action:
		return std::format("'{}' is not an action of the project any more", action);
	case inp::rebind_status::conflict:
		return std::format("{} is already used by '{}'", key, outcome.result.conflict_with);
	}
	return {};
}
