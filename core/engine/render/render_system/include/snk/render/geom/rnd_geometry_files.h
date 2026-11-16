#pragma once
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_tag.h>
#include <snk/common/snk_api.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace res { class resource_system; }

// The writer of a geometry desc and its binary file (EPIC-79, #1068): vertices and indices go to `<name>.geom.bin`, the
// desc keeps their ranges. Every writer of a *.geom.desc file goes through it.
namespace rnd
{
	// The __version a desc with ranges is written in: an older engine refuses it instead of reading a range as an array.
	inline constexpr int geometry_binary_version = 2;

	// The primitives of a geometry, as the importer found them. Only triangles get a binary file; any other stays in JSON
	// arrays (the form before EPIC-79) with the key "topology" naming it.
	enum class geometry_topology : std::uint8_t
	{
		triangles,
		lines,
		points,
		mixed,
	};

	// The key of an arrays desc that names a topology other than triangles; a desc without it holds triangles.
	inline constexpr std::string_view geometry_topology_key = "topology";
	SNK_API std::string_view topology_name(geometry_topology topology);
	// What "topology" of `desc` names: triangles without the key, mixed for a name this engine does not know.
	SNK_API geometry_topology topology_of(const boost::json::object& desc);

	struct geometry_file_options
	{
		// Written after "__type" and "__version" ("__parent", "__guid", ...); the writer's own keys in it are left out.
		boost::json::object header;
		// Writes the key "bounds" (empty): the loader builds the bounds again from the data, they are never stored.
		bool bounds = false;
		// Given by the caller (the importer knows it): anything but triangles is refused, it is written as arrays.
		geometry_topology topology = geometry_topology::triangles;
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

	// Why `geometry` cannot be written as `file` with its binary file; empty when it can. The loader's checks
	// (check_geometry_data: index past the vertices, a position or uv not finite, bytes off the stride, a partial triangle)
	// and: a topology other than triangles, no vertices, no indices (drawing is indexed), `file` not <name>.<ext>.desc.
	SNK_API std::string geometry_write_problem(const res::tag& file, const geometry_desc& geometry,
		const geometry_file_options& options = {});

	// The pair for `geometry` named `file`, nothing written; refused (`error`) as geometry_write_problem says. Vertices
	// are "f32xN" when every attribute is float, else "u8"; indices "u32".
	SNK_API geometry_files encode_geometry_files(const res::tag& file, const geometry_desc& geometry,
		const geometry_file_options& options = {});

	// Encodes and writes the pair through res::desc_binary::save, under the save lock from the staged binary file through
	// the desc to the commit, then signals the desc changed (it depends on the binary file: what was built from either
	// reloads). A commit that fails twice takes the desc back (the previous one, or none) and then the staged file; when
	// that fails too, `left` gets the desc and the staged file, which then belong together. A save that stopped before
	// the desc gives `left` the staged file. A protocol without layers (memory:// of the tests) cannot commit. Empty when
	// written.
	SNK_API std::string write_geometry_files(res::resource_system& system, const res::tag& file, const geometry_desc& geometry,
		const geometry_file_options& options = {}, std::vector<res::tag>* left = nullptr);

	// The data of the geometry desc `desc` (the text of `file`) without the cache: arrays as they are, or the ranges of the
	// binary file beside `file` with their hashes checked; bounds when the file has them. Empty when read into `out`.
	SNK_API std::string read_geometry_file(res::resource_system& system, const res::tag& file, const boost::json::object& desc,
		geometry_desc& out);

	// The same for a pair held in memory, nothing read or stored (the decoder's answer, #1068 PR3b): `binary` is the file
	// beside `file`, at most res::desc_binary::max_file_bytes, every range checked as the loader checks it and every hash
	// computed here. A desc with arrays takes no binary file. Empty when read into `out`.
	SNK_API std::string decode_geometry_files(const res::tag& file, const boost::json::object& desc,
		std::span<const std::byte> binary, geometry_desc& out);

	// Whether a geometry desc holds its vertices as a JSON array (the form before EPIC-79).
	SNK_API bool holds_geometry_arrays(const boost::json::object& desc);
}
