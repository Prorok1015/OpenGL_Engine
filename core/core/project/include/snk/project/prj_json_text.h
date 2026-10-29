#pragma once
#include <string>
#include <boost/json/value.hpp>
#include <snk/common/snk_api.h>

namespace prj
{
	// JSON for files people read and edit (the manifest, CMakeUserPresets.json): 4-space indents, `"key": value`,
	// one member or element per line, empty objects and arrays as `{}` / `[]`, object members in their stored order,
	// a trailing newline.
	SNK_API std::string to_indented_json(const boost::json::value& value);
}
