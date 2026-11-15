#pragma once
#include "edt_mesh_bake.h"
#include "edt_mesh_shapes.h"
#include <span>
#include <string>
#include <string_view>

namespace edt::mesh
{
	// The ready-made unit shapes of core/res/shapes (#746): a prefab node points at one of them and sets its size with the node's
	// scale. The files are what `base_shape_file_text` writes; a test keeps them from drifting away from the generator.
	struct base_shape
	{
		std::string_view name;
		shape_params params;
	};

	// __version of a geometry_desc file; a test keeps it equal to the engine's.
	inline constexpr int base_shape_file_version = 1;

	std::span<const base_shape> base_shapes();
	const base_shape* find_base_shape(std::string_view name);

	// res://shapes/<name>.geom.desc
	std::string base_shape_path(const base_shape& shape);
	// The whole file in the form of the repository's descs (pretty JSON, version, a newline at the end): deterministic, the bake of the generated mesh.
	std::string base_shape_file_text(const base_shape& shape);
	// The range of the one mesh in the file, as a mesh node writes it (vx_begin, vx_end, ind_begin, ind_end).
	material_range base_shape_range(const base_shape& shape);
}
