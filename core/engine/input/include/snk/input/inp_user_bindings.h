#pragma once
#include <snk/input/inp_actions.h>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The bindings a player changed (#736): per action name, the whole list of bindings that replaces the project's. The
// project's action_map is never edited; apply() builds the one the worlds read, with the actions in the project's order
// so the ids a module looked up stay valid.
namespace inp
{
	enum class rebind_status { applied, unknown_action, bad_slot, conflict, duplicate };

	struct rebind_result
	{
		rebind_status status = rebind_status::applied;
		// With `conflict`: the action that already has the binding. `duplicate`: the action has it in another slot.
		std::string conflict_with{};
		bool ok() const { return status == rebind_status::applied; }
		// With `allow_conflict`: applied, and this is the action that has the binding too (empty: no conflict).
		bool conflicted() const { return ok() && !conflict_with.empty(); }
	};

	// A binding two actions of a map share (the player's file or an edit of the desc can make one; rebind cannot).
	struct binding_conflict
	{
		action_binding binding{};
		std::string first;
		std::string second;
		// The same input shared by the same two actions: thresholds do not tell conflicts apart.
		bool operator==(const binding_conflict& other) const
		{
			return binding.same_input(other.binding) && first == other.first && second == other.second;
		}
	};
	SNK_API std::vector<binding_conflict> find_binding_conflicts(const action_map& map);

	class SNK_API user_bindings
	{
	public:
		using overrides_map = std::map<std::string, std::vector<action_binding>, std::less<>>;

		// Puts `binding` at `slot` of the action's current bindings (slot == their count adds one). A binding another action
		// of the effective map already has is a conflict, one the action has in another slot a duplicate; nothing changes.
		// With `allow_conflict` the conflict is only reported (`status` conflict is returned as applied with `conflict_with` set):
		// the binding is put and both actions keep it; a duplicate is refused always.
		rebind_result rebind(const action_map& project, std::string_view action, std::size_t slot, action_binding binding,
			bool allow_conflict);
		// The export of 3.16 (a module built for it imports this symbol): a conflict is refused.
		rebind_result rebind(const action_map& project, std::string_view action, std::size_t slot, action_binding binding);
		// Takes the binding at `slot` out of the action's current bindings; `bad_slot` for a slot that is not there.
		rebind_result remove(const action_map& project, std::string_view action, std::size_t slot);
		// Drops the change of one action; false when it had none.
		bool reset(std::string_view action);
		void reset_all() { m_overrides.clear(); }
		// Replaces the action's bindings as they are (loading a file); no checks.
		void set(std::string action, std::vector<action_binding> bindings);

		bool empty() const { return m_overrides.empty(); }
		const overrides_map& overrides() const { return m_overrides; }

		// The project's map with the changed actions swapped in. A change of an action the project does not have is ignored.
		action_map apply(const action_map& project) const;

	private:
		overrides_map m_overrides;
	};
}
