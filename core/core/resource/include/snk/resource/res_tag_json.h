#pragma once
// JSON (de)serialization for res::tag, split out of res_tag.h (EPIC-49 US-49-4).
// res_tag.h is included by ~35 files but only a handful actually (de)serialize tags via
// boost::json. Keeping <boost/json.hpp> + the tag_invoke customization points here means the
// lightweight res::tag value type no longer drags Boost.JSON into every includer.
// Include this header only in TUs that call json::value_from / json::value_to on a res::tag.
#include <snk/resource/res_tag.h>
#include <boost/json.hpp>
#include "snk_api.h"

namespace json = boost::json;

namespace res
{
	SNK_API void tag_invoke(json::value_from_tag, json::value& out, const res::tag& c);
	SNK_API res::tag tag_invoke(json::value_to_tag<res::tag>, const json::value& obj);
}
