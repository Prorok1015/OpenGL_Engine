#pragma once
#include "edt_mesh_bake.h"
#include "edt_mesh_shapes.h"
#include <optional>
#include <string>
#include <string_view>

namespace edt::mesh
{
	inline constexpr std::string_view poly_file_suffix = ".poly.desc";
	inline constexpr std::string_view geometry_file_suffix = ".geom.desc";
	inline constexpr int poly_file_version = 1;
	// __version of a geometry_desc file; a test keeps it equal to the engine's.
	inline constexpr int geometry_file_version = 1;

	// The two files of a parametric shape (#746): `poly` is the editable source (editor only, nothing at run time refers to it),
	// `geometry` is what the engine loads. Both are in the form of the repository's descs (pretty JSON, version, a newline at the end).
	struct shape_asset_files
	{
		std::string poly;
		std::string geometry;
		std::size_t vertices = 0;
		std::size_t indices = 0;
	};

	struct shape_asset_result
	{
		std::optional<shape_asset_files> files;
		std::string error;
	};

	// Deterministic: the same parameters give the same bytes.
	shape_asset_result build_shape_asset(const shape_params& params);

	// The whole geometry file of a baked mesh, as a repository desc.
	std::string geometry_file_text(const rnd::geometry_desc& geometry);

	// Whether a poly file may be rebuilt from its parameters. The mesh counts as edited by hand when the file has no generator or its
	// mesh (read as data, so whitespace and key order do not count) differs from what its generator makes now: nobody sets a flag, so an editor that changes a mesh without clearing anything
	// cannot be forgotten.
	enum class shape_source { generated, edited, unreadable };

	struct shape_source_check
	{
		shape_source state = shape_source::unreadable;
		std::optional<shape_params> generator;
		std::string error;
	};

	shape_source_check check_shape_source(std::string_view poly_text);
}
