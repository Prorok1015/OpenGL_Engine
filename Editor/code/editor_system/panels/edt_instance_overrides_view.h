#pragma once
#include <snk/scene/level/scn_prefab_desc.h>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	struct instance_overrides_hooks
	{
		// "Edit Prefab": opens the prefab the node instantiates in the prefab editor. Empty: no button.
		std::function<void(const res::tag&)> open_prefab = {};
	};

	// One line for a person about an override record: which node it is on, what it changes and to what, e.g.
	// "a1 > b2 : stat /hp = 12", "a1 : remove component light", "add node 'extra'". Ids are shortened (leading zeros dropped), a long
	// value is cut. Text that comes from a file stays text: nothing here is a format string.
	std::string describe_override_record(const scn::override_record& record);

	// What the Inspector shows of an instance node (a node that instantiates a prefab, or has override records): the prefab it
	// instantiates with "Edit Prefab", and its override records (#149), each with "Revert" (the field is the prefab's again).
	// A record in `stale` (find_stale_records of `node`; null: none known) is greyed with the reason and says "Remove"; "Clear stale
	// overrides" drops them all. "Revert All" drops every record. Returns true when `node.overrides` changed; the caller commits.
	bool draw_instance_overrides(scn::prefab_desc::prefab_node& node, const std::vector<scn::stale_override>* stale,
		const instance_overrides_hooks& hooks);
}
