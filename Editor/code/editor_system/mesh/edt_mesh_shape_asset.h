#pragma once
#include "edt_mesh_bake.h"
#include "edt_mesh_shapes.h"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/model_codec/res_tag.h>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt::mesh
{
	inline constexpr std::string_view poly_file_suffix = ".poly.desc";
	inline constexpr std::string_view geometry_file_suffix = ".geom.desc";
	inline constexpr int poly_file_version = 1;
	// __version of a geometry_desc file with ranges (EPIC-79); a test keeps it equal to the engine's.
	inline constexpr int geometry_file_version = 2;

	// A geometry as its two files (EPIC-79, #1068): the desc with the ranges in the form of the repository's descs (pretty JSON,
	// version, a newline at the end) and the binary file beside it, as rnd::encode_geometry_files makes them.
	struct geometry_pair
	{
		std::string desc;
		std::vector<std::byte> binary;
	};

	// `error` is empty when `geometry` is encoded as the files of `desc_file` (a <name>.geom.desc tag): the content does not depend on the name.
	geometry_pair encode_geometry_pair(const res::tag& desc_file, const rnd::geometry_desc& geometry, std::string& error);

	// The files of a parametric shape (#746): `poly` is the editable source (editor only, nothing at run time refers to it),
	// `geometry` and `binary` are what the engine loads (the desc and the .geom.bin beside it). `data` is the baked geometry
	// that rnd::write_geometry_files writes as that pair. The poly file is in the form of the repository's descs.
	struct shape_asset_files
	{
		std::string poly;
		std::string geometry;
		std::vector<std::byte> binary;
		rnd::geometry_desc data;
		std::size_t vertices = 0;
		std::size_t indices = 0;
	};

	struct shape_asset_result
	{
		std::optional<shape_asset_files> files;
		std::string error;
	};

	// The number of vertices the bake of a shape has, from its parameters alone: what lets a request that cannot fit a file be
	// refused before a mesh of millions of faces is made. A test keeps it equal to the real count.
	std::size_t estimate_shape_vertices(const shape_params& params);

	// Deterministic: the same parameters give the same bytes.
	// `geometry_file`: the <name>.geom.desc tag the geometry will be written as.
	shape_asset_result build_shape_asset(const shape_params& params, const res::tag& geometry_file);

	// The range of vertices and indices of a geometry desc written by build_shape_asset (the layout of the bake, one mesh), as its
	// ranges say (the sizes of the vertex and index chunks; arrays are counted too): what a mesh node that draws the whole file
	// holds. `error` says why a desc is not one.
	std::optional<material_range> geometry_file_range(std::string_view text, std::string& error);

	// Empty when `scale` is a size for a shape node: every component finite and greater than 0 (a negative one turns the mesh inside out).
	std::string shape_scale_problem(const glm::vec3& scale);

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
