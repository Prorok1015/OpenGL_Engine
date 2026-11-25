#pragma once
#include <string_view>

namespace edt
{
	// The GLSL editor registers one asset kind per suffix, `shader_source.<suffix>` (the registry matches a suffix as written). To the asset
	// browser's filter and to an agent's asset.query they are the one kind `shader`, as before the editor existed (#1202).
	inline constexpr std::string_view k_shader_source_kind_prefix = "shader_source";

	// The kind a person (the Asset Browser's filter) and an agent (asset.query) see for an asset kind of the registry: the kinds that
	// only tell how a file opens fold into the kind they belong to. Any other id is its own kind.
	inline std::string_view outward_kind_name(std::string_view registered_id)
	{
		if (registered_id.starts_with(k_shader_source_kind_prefix)) return "shader";
		// A texture desc (#1205) and a picture double-clicked into the texture viewer are textures.
		if (registered_id == "texture_2d_desc" || registered_id.starts_with("picture.")) return "texture";
		return registered_id;
	}
}
