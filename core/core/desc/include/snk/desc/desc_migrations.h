#pragma once
#include <string>
#include <string_view>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <snk/common/snk_api.h>

// Field moves of a migration step (EPIC-58 section 3.5, DESC_SCHEMA section 2.6), for C++ steps and for the "migrations" of a runtime
// schema alike. A pointer is a JSON pointer from the root of the desc object ("/hp", "/stats/hp"); the header keys
// (__type, __version, __guid) are not a field and are refused. A malformed pointer throws std::invalid_argument.
namespace desc::migrations
{
	// Moves the value at `from` to `to`. Nothing to move is no step; a value already at `to` throws std::runtime_error
	// rather than be overwritten.
	SNK_API void rename(boost::json::object& object, std::string_view from, std::string_view to);

	// Removes the value at `pointer`, if there is one.
	SNK_API void remove(boost::json::object& object, std::string_view pointer);

	// Puts `value` at `pointer` where nothing is there yet; a value already there (null too) stays.
	SNK_API void set_default(boost::json::object& object, std::string_view pointer, boost::json::value value);
}

namespace desc
{
	// What is wrong with the "migrations" of a runtime schema of `version`, or empty when nothing is:
	//   [{ "from": 1, "rename": { "/hp": "/health" } }, { "from": 2, "remove": ["/legacy"], "set_default": { "/armor": 0 } }]
	// Steps run from 1 without a gap and lead to the schema's version (version == 1 + their number). Within a step the
	// renames run first, then the removals, then the defaults.
	SNK_API std::string schema_migrations_problem(const boost::json::array& migrations, int version);
}
