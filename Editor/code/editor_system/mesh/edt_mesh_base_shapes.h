#pragma once
#include "edt_mesh_bake.h"
#include "edt_mesh_shapes.h"
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace edt::mesh
{
	// The ready-made unit shapes of core/res/shapes (#746): a prefab node points at one of them and sets its size with the node's
	// scale. The files are what `base_shape_files` makes; a test keeps them from drifting away from the generator.
	// Every shape is centred on the origin; a capsule has diameter 1 and its height is the straight part plus 1, so the extents are (1, 1.5, 1),
	// (1, 2, 1) and (1, 3, 1) for capsule_s, capsule_m and capsule_l.
	struct base_shape
	{
		std::string_view name;
		shape_params params;
	};

	// __version of a geometry_desc file; a test keeps it equal to the engine's.
	inline constexpr int base_shape_file_version = 2;

	std::span<const base_shape> base_shapes();
	const base_shape* find_base_shape(std::string_view name);

	// res://shapes/<name>.geom.desc
	std::string base_shape_path(const base_shape& shape);

	// A shape as its two files (EPIC-79, #1129): the desc with the ranges, in the form of the repository's descs (pretty JSON,
	// version, a newline at the end), and <name>.geom.bin beside it, both as rnd::encode_geometry_files makes them from the
	// bake of the generated mesh. Deterministic: the encoder takes nothing but the data, in a fixed order.
	struct base_shape_pair
	{
		std::string desc;
		std::vector<std::byte> binary;
	};

	base_shape_pair base_shape_files(const base_shape& shape);
	// The range of the one mesh in the file, as a mesh node writes it (vx_begin, vx_end, ind_begin, ind_end).
	material_range base_shape_range(const base_shape& shape);
}
