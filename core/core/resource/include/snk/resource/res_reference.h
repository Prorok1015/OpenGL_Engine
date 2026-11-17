#pragma once
#include <optional>
#include <boost/json/value.hpp>
#include <snk/common/snk_api.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_tag.h>

namespace json = boost::json;

namespace res
{
	// A reference to an asset as a file writes it (EPIC-57 section 7.1): a path string, or {"guid": ..., "path": ...}.
	// Either half may be missing (nil guid, empty path); resource_system::resolve_reference turns it into the tag to load.
	struct reference
	{
		asset_id guid;
		tag path;
	};

	// A string (an empty one is the empty reference), or an object with "guid" and/or "path" and no other key, the guid
	// a UUID that is not nil. Anything else is nullopt. Not inline, so modules read whatever form the engine accepts.
	SNK_API std::optional<reference> reference_from_json(const json::value& value);
}
