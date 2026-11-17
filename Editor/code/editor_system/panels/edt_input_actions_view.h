#pragma once
#include <snk/editor/edt_problem_list.h>
#include <snk/game/gs_input_actions.h>
#include <snk/input/inp_actions.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// ImGui-free part of the Input Actions panel (US-542-C, WORLD_BUS.md 4.6): the actions with their bindings as table rows,
// what the player's changes did to them, the state of "press the new key", and the words for what a rebind answered.
namespace edt::input_actions_view
{
	// "A", "Mouse LEFT", "Pad A", "Axis LeftX-": what the table shows for a binding.
	std::string binding_text(const inp::action_binding& binding);

	struct binding_row
	{
		std::string text;
		// Differs from what the project has at this slot (a change of the player's, or a slot the project does not have).
		bool changed = false;
	};

	struct action_row
	{
		std::string name;
		std::vector<binding_row> bindings;
		// Some binding of it differs from the project's, so Reset has something to do.
		bool changed = false;
		// The other actions that have one of its bindings too, each once, in map order.
		std::vector<std::string> shares_with;
	};

	// The rows of the actions of `effective` (the order of the project's map); `project` tells what changed, `conflicts` which
	// actions share a binding.
	std::vector<action_row> build_rows(const inp::action_map& project, const inp::action_map& effective,
		const std::vector<inp::binding_conflict>& conflicts);

	// One Problems entry per conflict (a warning), as "Input actions 'Jump' and 'Fire' share A".
	std::vector<problem> conflict_problems(const std::vector<inp::binding_conflict>& conflicts);

	// The slot that is waiting for the player's next key or button. `slot` equal to the action's binding count adds one.
	class capture
	{
	public:
		void begin(std::string action, std::size_t slot);
		void cancel() { m_active = false; }
		bool active() const { return m_active; }
		const std::string& action() const { return m_action; }
		std::size_t slot() const { return m_slot; }
		// True for the row and slot that wait, so the table can show "press a key" there.
		bool waits_on(std::string_view action, std::size_t slot) const { return m_active && m_action == action && m_slot == slot; }

	private:
		bool m_active = false;
		std::string m_action;
		std::size_t m_slot = 0;
	};

	// What to tell the player after a rebind or a removal; empty when there is nothing to say (a plain success).
	std::string outcome_text(const gs::rebind_outcome& outcome, std::string_view action, const inp::action_binding* binding);
}
