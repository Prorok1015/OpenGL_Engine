#pragma once
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_tag.h>
#include <snk/common/snk_api.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <string>
#include <vector>

namespace res { class resource_system; }

// The writer of a geometry desc and its binary file (EPIC-79, #1068): vertices and indices go to `<name>.geom.bin`, the
// desc keeps their ranges. Every writer of a *.geom.desc file goes through it.
namespace rnd
{
	// The __version a desc with ranges is written in: an older engine refuses it instead of reading a range as an array.
	inline constexpr int geometry_binary_version = 2;

	struct geometry_file_options
	{
		// Written after "__type" and "__version" ("__parent", "__guid", ...); the writer's own keys in it are left out.
		boost::json::object header;
		// Writes the key "bounds" (empty): the loader builds the bounds again from the data, they are never stored.
		bool bounds = false;
	};

	struct geometry_files
	{
		// The desc as compact JSON, its ranges filled.
		std::vector<std::byte> desc;
		// The binary file beside it.
		std::vector<std::byte> binary;
		// Empty when encoded.
		std::string error;

		explicit operator bool() const { return error.empty(); }
	};

	// The pair for `geometry` named `file`, nothing written. Refused (`error`) when the data fails the loader's checks
	// (check_geometry_data), there are no vertices or `file` is not named <name>.<ext>.desc. Vertices are "f32xN" when every
	// attribute is float, else "u8"; indices "u32".
	SNK_API geometry_files encode_geometry_files(const res::tag& file, const geometry_desc& geometry,
		const geometry_file_options& options = {});

	// Encodes and writes the pair through res::desc_binary::save, which holds the save lock from the staged binary file
	// through the desc to the commit. A protocol without layers (memory:// of the tests) cannot commit. Empty when written.
	SNK_API std::string write_geometry_files(res::resource_system& system, const res::tag& file, const geometry_desc& geometry,
		const geometry_file_options& options = {});

	// The data of the geometry desc `desc` (the text of `file`) without the cache: arrays as they are, or the ranges of the
	// binary file beside `file` with their hashes checked; bounds when the file has them. Empty when read into `out`.
	SNK_API std::string read_geometry_file(res::resource_system& system, const res::tag& file, const boost::json::object& desc,
		geometry_desc& out);

	// Whether a geometry desc holds its vertices as a JSON array (the form before EPIC-79).
	SNK_API bool holds_geometry_arrays(const boost::json::object& desc);
}
