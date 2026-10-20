#pragma once
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema.h>
#include <boost/json/object.hpp>

namespace desc
{
	// JSON Schema (draft 2020-12 vocabulary) of the JSON object a type_schema reads and writes, for agents and tools
	// that want to build a valid value. Fields sit under their keys (a pointer such as "/limits/min" becomes nested
	// "properties"); no field is "required" because a missing key keeps the default. Unknown keys are not forbidden:
	// the reader ignores them. Structure and enumeration fields are resolved through `resolve` (an unknown nested
	// type becomes a plain object, an unknown enum a string). Extra keywords: "x-version", "x-kind" on the root and
	// "x-degrees" on a field that is stored in degrees.
	SNK_API boost::json::object to_json_schema(const type_schema& schema, const schema_registry& resolve);
}
