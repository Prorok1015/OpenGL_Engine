#pragma once
#include <boost/json.hpp>
#include <functional>
#include <string>
#include <string_view>

namespace desc { struct type_schema; class schema_registry; }

namespace edt
{
	// What a record of an instance holds for an embedded desc (EPIC-82 8.1, #1403): `{__type, __version, __parent, ...own fields}`, the same bytes
	// for the same meaning. The memory tag of an embedded desc is hashed from the bytes of this value (desc_system::make_mem_tag_deterministic), so
	// two edits that mean one thing must give one value, or the renderer builds two materials.
	//
	// The form of a reference: a tag may be written as a string or as `{guid, path}` (what the save writes, desc_system::write_references). `normalize`
	// (unset: as is) maps a reference value to the one form the caller wants; it is applied to every res_tag / desc_ref field and element, so the value
	// before a save and the one read back after it are the same bytes. The caller gives it the same rule as the save (write_references).
	using reference_normalizer = std::function<boost::json::value(const boost::json::value&)>;

	// `own`: the fields the instance sets; `base`: what the parent file gives (the merged chain without `own`). A field that equals its base value (after
	// the same normalising) is left out, so a value that equals the parent is not fixed and follows the parent afterwards: it is the VALUES EQUAL TO THE BASE
	// that are not written, not the defaults. Objects are compared key by key, except what is one value: an inline desc (it has a `__type`), a reference
	// `{guid, path}` and a map field (a key that is gone cannot be said by a diff) are written whole. Arrays are whole.
	//
	// Order: the fields of the schema in its order (a field is matched by its key, a key that is a pointer such as `uniforms/albedo` is not matched), then
	// the other keys alphabetically, and the same below an object the schema does not describe. Numbers: an integral value is an integer (`1`, never `1.0`
	// or `1E0`); a float32 field (f32, vectors, quat, color) takes the shortest text of its float, so 0.1f is 0.1. `parent`: the path of the file this value
	// is the variant of, written as a string; empty: no `__parent`. No ImGui, no resource system.
	boost::json::object canonical_embedded_value(const desc::schema_registry& schemas, const desc::type_schema& schema, const boost::json::object& own,
		const boost::json::object& base, int version, std::string_view parent, const reference_normalizer& normalize = {});
}
