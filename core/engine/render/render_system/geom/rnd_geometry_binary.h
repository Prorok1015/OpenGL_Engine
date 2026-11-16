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

// The binary data of a geometry_desc (EPIC-79, #1068): what deserialize records for the geometry adapter and the
// checks of the data before it reaches the GPU. Kept beside the desc by its address, so geometry_desc keeps its layout.
namespace rnd::detail
{
	enum class geometry_binary_state : std::uint8_t
	{
		// Arrays only, or nothing read yet.
		none,
		// Ranges recorded, the data not read: a desc read through the generic desc adapter.
		not_loaded,
		// The geometry adapter filled the vectors from the binary file.
		loaded,
	};

	struct geometry_binary_ranges
	{
		std::optional<res::binary_range> vertices;
		std::optional<res::binary_range> indices;
		// The file has "bounds": they are built again from the checked data, never read.
		bool wants_bounds = false;
	};

	geometry_binary_state binary_state_of(const geometry_desc& geometry);
	// The ranges deserialize recorded; nullopt unless the state is not_loaded.
	std::optional<geometry_binary_ranges> binary_ranges_of(const geometry_desc& geometry);
	void set_binary_ranges(const geometry_desc& geometry, geometry_binary_ranges ranges);
	void set_binary_loaded(const geometry_desc& geometry);
	void forget_binary(const geometry_desc& geometry);

	// Throws res::load_error naming `file` and the JSON path: a stride of 0, vertex bytes off the stride, an index count
	// off 3, an index past the vertices, a position or uv that is not finite.
	void check_geometry_data(const res::tag& file, const geometry_desc& geometry);
	// Arrays of any topology the loader takes (lines, points too): vertex bytes off the stride, an index past the vertices,
	// a position or uv that is not finite. Nothing for a geometry without vertices or with an empty layout.
	void check_geometry_arrays(const res::tag& file, const geometry_desc& geometry);
	// Bounds (uv of each triangle, as the importer makes them) and the tree, from data check_geometry_data passed.
	void rebuild_bounds(geometry_desc& geometry);

	// What geometry_desc::deserialize reads: the layout, arrays as they are, ranges recorded (state not_loaded).
	void read_geometry_json(geometry_desc& geometry, const boost::json::object& resource);
	// The "layout" object of a desc.
	boost::json::value layout_json(const driver::BufferLayout& layout);
	// The recorded ranges read from the binary file beside `file` (its layer), checked, copied into the vectors, bounds
	// built again when the file has the key. Throws res::load_error naming the JSON path.
	void load_geometry_binary(res::resource_system& system, const res::tag& file, geometry_desc& geometry, bool verify_hash);
	// The same from `file`, the bytes of the binary file beside `tag` held in memory (an answer not yet stored): at most
	// res::desc_binary::max_file_bytes, every hash checked.
	void decode_geometry_binary(const res::tag& tag, std::span<const std::byte> file, geometry_desc& geometry);
}
