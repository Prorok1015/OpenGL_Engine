#pragma once
#include <string_view>

namespace edt
{
	// The GLSL editor registers one asset kind per suffix, `shader_source.<suffix>` (the registry matches a suffix as written). To the asset
	// browser's filter and to an agent's asset.query they are the one kind `shader`, as before the editor existed (#1202).
	inline constexpr std::string_view k_shader_source_kind_prefix = "shader_source";

	inline std::string_view outward_kind_name(std::string_view registered_id)
	{
		return registered_id.starts_with(k_shader_source_kind_prefix) ? std::string_view{ "shader" } : registered_id;
	}
}
