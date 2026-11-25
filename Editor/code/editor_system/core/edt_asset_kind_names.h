#pragma once
#include <string_view>

namespace edt
{
	// The kind a person (the Asset Browser's filter) and an agent (asset.query) see for an asset kind of the registry: the
	// kinds that only tell how a file opens fold into the kind they belong to. Any other id is its own kind.
	inline std::string_view outward_kind_name(std::string_view registry_id)
	{
		// A texture desc (#1205) and a picture double-clicked into the texture viewer are textures.
		if (registry_id == "texture_2d_desc") return "texture";
		if (registry_id.starts_with("picture.")) return "texture";
		return registry_id;
	}
}
