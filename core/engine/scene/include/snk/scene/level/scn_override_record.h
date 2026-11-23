#pragma once
#include <boost/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

namespace desc { class desc_system; }

namespace scn {

	// Component key that addresses a node's position/rotation/scale instead of a real component.
	inline constexpr std::string_view transform_pseudo_component = "transform";

	enum class override_op {
		set,      // replace the value at `path` inside `component` (path "" adds the component or merges into it)
		remove,   // drop `component`; without a component drop the whole node
		add_node, // `value` is a node; appended under `target` (merged into the child with the same id if there is one)
	};

	// One explicit change an instance makes to the prefab it instantiates. A node or field that has no record
	// is inherited, a record overrides even when it equals the default, so "back to the default value" is expressible.
	struct override_record {
		std::vector<uint64_t> target;  // node id path from the root of the instance; empty = the root itself
		std::string component;         // component key or transform_pseudo_component; empty for node-level records
		std::string path;              // RFC 6901 pointer inside the component
		boost::json::value value;
		override_op op = override_op::set;
		// The version of the component's type the set was written against (#1255): its path and value follow the steps
		// of that type from there. 0 = not known (a file before prefab v5, or the transform pseudo-component): applied
		// as written.
		int type_version = 0;
	};

	// Reads {target, op, component, path, value, version}; a malformed record is logged and skipped.
	SNK_API std::optional<override_record> parse_override_record(const boost::json::value& json);
	SNK_API boost::json::object override_record_to_json(const override_record& record);

	// Canonical form for saving: records that a later one makes irrelevant are dropped (a record at pointer P
	// absorbs earlier records under P, removing a component or node absorbs everything inside it), the rest
	// is sorted by (target, component, path). Applying the result is equivalent to applying the input.
	SNK_API void canonicalize_overrides(std::vector<override_record>& records);

	enum class record_migration {
		kept,       // nothing to do: written against the current version, or not a set of a typed component
		moved,      // the steps moved or converted the value: path, value and version are rewritten
		dropped,    // a step removed the field: the edit is gone
		duplicated, // a step put the value in more than one place: the record is left as it is and must not be applied
		failed,     // a step threw, or changed an array the path goes through: the record is left as it is
	};

	struct record_migration_result {
		record_migration outcome = record_migration::kept;
		std::string reason; // what happened, for dropped, duplicated and failed
	};

	// Brings a set record written against an older version of `type` (the type of the component it edits) up to the current
	// one (#1255) through the same steps an object of that type takes, C++ and declarative alike: a sparse object holding
	// the value at the record's path goes through the steps next to the same object without the value, and what differs
	// after is where the value is now. A record without a version (0) is taken as written against the current one and gets
	// that version. On kept and moved the record is at the current version; on the others it is left untouched.
	SNK_API record_migration_result migrate_override_record(const desc::desc_system& descs, std::string_view type,
		override_record& record);
}
