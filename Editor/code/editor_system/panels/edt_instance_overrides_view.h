#pragma once
#include "edt_override_edit.h"
#include <snk/scene/level/scn_prefab_desc.h>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	struct instance_overrides_hooks
	{
		// "Edit Prefab": opens the prefab the node instantiates in the prefab editor. Empty: no button.
		std::function<void(const res::tag&)> open_prefab = {};
		// "Apply to Prefab": writes the records into the file of the prefab the node instantiates (override_edit::fold_record, one level:
		// the prefab, not a prefab nested in it). Returns why it was not done, empty when the file is written. Empty function: no button.
		std::function<std::string(const res::tag&, const std::vector<scn::override_record>&)> apply_to_prefab = {};
		std::function<std::size_t(const res::tag&)> instances_in_level = {};
		// False while the Inspector shows something else than the open level (a prefab of the prefab editor): no Apply buttons then.
		std::function<bool()> can_apply = {};
	};

	// What the section keeps between frames: the apply a person was asked about, and why the last one failed.
	struct instance_overrides_state
	{
		static constexpr std::size_t all_records = static_cast<std::size_t>(-1);
		std::optional<std::size_t> asking = std::nullopt;
		bool open_requested = false;
		// The node and its records as they were when the question was asked: another node, or other records, and it is dropped.
		std::uint64_t asked_node = 0;
		std::vector<std::string> asked_records;
		std::string error;
	};

	// The question before records move into a prefab file: it names the prefab, the instances the open level has (others may exist
	// elsewhere) and how many records go, and says that it cannot be undone.
	std::string apply_to_prefab_question(const res::tag& prefab, std::size_t instances, std::size_t records);

	// One line for a person about an override record: which node it is on, what it changes and to what, e.g.
	// "a1 > b2 : stat /hp = 12", "a1 : remove component light", "add node 'extra'". Ids are shortened (leading zeros dropped), a long
	// value is cut. Text that comes from a file stays text: nothing here is a format string.
	std::string describe_override_record(const scn::override_record& record);

	// What the Inspector shows of an instance node (a node that instantiates a prefab, or has override records): the prefab it
	// instantiates with "Edit Prefab", and its override records (#149), each with "Revert" (the field is the prefab's again).
	// A record in `stale` (view_cache::stale of `node`: a node, component or field the prefab no longer has; null: none known) is greyed with the reason and says "Remove"; "Clear stale
	// overrides" drops them all. "Revert All" drops every record. "Apply" (a value that was set, not stale; "Apply All" for all of them)
	// asks first, then moves the record into the prefab file through `hooks.apply_to_prefab` and drops it from the node.
	// Returns true when `node.overrides` changed; the caller commits.
	bool draw_instance_overrides(scn::prefab_desc::prefab_node& node, const std::vector<override_edit::stale_record>* stale,
		const instance_overrides_hooks& hooks, instance_overrides_state& state);
}
