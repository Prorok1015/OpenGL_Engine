#pragma once
#include <boost/json/value.hpp>
#include <optional>
#include <string>
#include <snk/common/snk_api.h>

namespace cmd::json_schema
{
	// A small subset of JSON Schema: type (name or list of names; integer, number, string, boolean, object,
	// array, null), required, properties, additionalProperties (bool), enum, minimum, maximum, minLength
	// (strings, in characters), plus description and title, which only document. Kept apart from the command
	// registry so the desc schemas (#128) can replace it.

	// Why `schema` is not a usable schema; empty if it is. Any other keyword is refused, so a schema never
	// validates less than it says. `required` names must be listed in `properties`.
	SNK_API std::optional<std::string> check_schema(const boost::json::value& schema);

	// Why `instance` does not match `schema` (with the path of the offending value); empty if it matches.
	// `schema` must have passed check_schema.
	SNK_API std::optional<std::string> validate(const boost::json::value& schema, const boost::json::value& instance);
}
