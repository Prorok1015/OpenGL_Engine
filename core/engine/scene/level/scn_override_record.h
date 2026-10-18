#pragma once
#include <boost/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

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
	};

	// Reads {target, op, component, path, value}; a malformed record is logged and skipped.
	SNK_API std::optional<override_record> parse_override_record(const boost::json::value& json);
	SNK_API boost::json::object override_record_to_json(const override_record& record);

	// Canonical form for saving: records that a later one makes irrelevant are dropped (a record at pointer P
	// absorbs earlier records under P, removing a component or node absorbs everything inside it), the rest
	// is sorted by (target, component, path). Applying the result is equivalent to applying the input.
	SNK_API void canonicalize_overrides(std::vector<override_record>& records);
}
