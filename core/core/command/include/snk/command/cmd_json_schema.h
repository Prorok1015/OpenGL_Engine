#pragma once
#include <boost/json/value.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <snk/common/snk_api.h>

namespace cmd::json_schema
{
	// A small subset of JSON Schema: type (name or list of names; integer, number, string, boolean, object,
	// array, null), required, properties, additionalProperties (bool), minProperties (objects), items (one schema
	// for every element), enum, minimum, maximum, minLength (strings, in characters), plus description and title,
	// which only document. Kept apart from the command
	// registry so the desc schemas (#128) can replace it.

	// The integer a value stands for: the schema type "integer" accepts a uint64, an int64 and a double with no
	// fraction (a JSON writer may send 5.0), so a command reads its integer arguments through this and never calls
	// as_int64() on them. Nullopt when the value is none of those or does not fit an int64.
	SNK_API std::optional<std::int64_t> integer_of(const boost::json::value& value);

	// Why `schema` is not a usable schema; empty if it is. Any other keyword is refused, so a schema never
	// validates less than it says. `required` names must be listed in `properties`.
	SNK_API std::optional<std::string> check_schema(const boost::json::value& schema);

	// Why `instance` does not match `schema` (with the path of the offending value); empty if it matches.
	// `schema` must have passed check_schema.
	SNK_API std::optional<std::string> validate(const boost::json::value& schema, const boost::json::value& instance);
}
