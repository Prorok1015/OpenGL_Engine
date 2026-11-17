#pragma once
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_tag.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <optional>
#include <span>

namespace res { class resource_system; }

// The binary data of a geometry_desc (EPIC-79, #1068): what deserialize records for the geometry adapter (the member
// geometry_desc::binary) and the checks of the data before it reaches the GPU.
namespace rnd::detail
{
	geometry_binary_state binary_state_of(const geometry_desc& geometry);
	// The ranges deserialize recorded; nullopt unless the state is not_loaded.
	std::optional<geometry_binary_ranges> binary_ranges_of(const geometry_desc& geometry);
	void set_binary_ranges(geometry_desc& geometry, geometry_binary_ranges ranges);
	void set_binary_loaded(geometry_desc& geometry);
	void forget_binary(geometry_desc& geometry);

	// Throws res::load_error naming `file` and the JSON path: what geometry_data_problem (rnd_geometry_encode.h) refuses of
	// triangles: a stride of 0, vertex bytes off the stride, an index count off 3, an index past the vertices, a position or
	// uv that is not finite.
	void check_geometry_data(const res::tag& file, const geometry_desc& geometry);
	// Arrays of any topology the loader takes (lines, points too): vertex bytes off the stride, an index past the vertices,
	// a position or uv that is not finite. Nothing for a geometry without vertices or with an empty layout.
	void check_geometry_arrays(const res::tag& file, const geometry_desc& geometry);
	// Bounds (uv of each triangle, as the importer makes them) and the tree, from data check_geometry_data passed.
	void rebuild_bounds(geometry_desc& geometry);

	// What geometry_desc::deserialize reads: the layout, arrays as they are, ranges recorded (state not_loaded).
	void read_geometry_json(geometry_desc& geometry, const boost::json::object& resource);
	// The recorded ranges read from the binary file beside `file` (its layer), checked, copied into the vectors, bounds
	// built again when the file has the key. Throws res::load_error naming the JSON path.
	void load_geometry_binary(res::resource_system& system, const res::tag& file, geometry_desc& geometry, bool verify_hash);
	// The same from `file`, the bytes of the binary file beside `tag` held in memory (an answer not yet stored): at most
	// res::desc_binary::max_file_bytes, every hash checked; no bounds are built (the caller encodes the data again).
	void decode_geometry_binary(const res::tag& tag, std::span<const std::byte> file, geometry_desc& geometry);
}
