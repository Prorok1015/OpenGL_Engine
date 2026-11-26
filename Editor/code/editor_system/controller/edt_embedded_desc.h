#pragma once
#include <boost/json.hpp>
#include <string>
#include <string_view>

namespace desc { struct type_schema; class schema_registry; }

namespace edt
{
	// The value of an embedded desc as a record of an instance holds it (EPIC-82 8.1, #1403): `{__type, __version, __parent, ...own fields}`, the
	// same bytes for the same meaning. The memory tag of an embedded desc is made from the bytes of this value (desc_system::make_mem_tag_deterministic),
	// so two edits that mean one thing must give one value or the renderer builds two materials.
	//
	// `own`: the fields the instance sets; `base`: what the parent file gives (the merged chain without `own`): a field that equals it says nothing
	// and is left out, an object is compared key by key. Keys follow the schema's order, keys the schema does not know follow them alphabetically
	// (so does everything below an object the schema does not describe); arrays stay as they are. `parent`: the path of the file the value is
	// the variant of, written as a string; empty: no `__parent`. No ImGui, no resource system.
	boost::json::object canonical_embedded_value(const desc::schema_registry& schemas, const desc::type_schema& schema, const boost::json::object& own,
		const boost::json::object& base, int version, std::string_view parent);
}
