#pragma once
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/model_codec/rnd_geometry_encode.h>
#include <snk/model_codec/res_tag.h>
#include <snk/common/snk_api.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace res { class resource_system; }

// The writer of a geometry desc and its binary file (EPIC-79, #1068): vertices and indices go to `<name>.geom.bin`, the
// desc keeps their ranges. Every writer of a *.geom.desc file goes through it.
namespace rnd
{
	// The data of `geometry`, for the encoders of rnd_geometry_encode.h (it must outlive the view).
	inline geometry_view geometry_view_of(const geometry_desc& geometry)
	{
		return geometry_view{ geometry.layout, geometry.vertices, geometry.indices, geometry.bounds };
	}

	// geometry_write_problem and encode_geometry_files of rnd_geometry_encode.h for the data of `geometry`.
	SNK_API std::string geometry_write_problem(const res::tag& file, const geometry_desc& geometry,
		const geometry_file_options& options = {});
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
	// computed here; no bounds are built from the binary file (the data is for encoding again). A desc with arrays takes no
	// binary file. Empty when read into `out`.
	SNK_API std::string decode_geometry_files(const res::tag& file, const boost::json::object& desc,
		std::span<const std::byte> binary, geometry_desc& out);

	// Whether a geometry desc holds its vertices as a JSON array (the form before EPIC-79).
	SNK_API bool holds_geometry_arrays(const boost::json::object& desc);

	// The bounds the loader builds from the data of `geometry` for a desc with ranges, which never stores them: one box of
	// the uv of each triangle. A migration compares stored bounds with these before it lets them go (#1129).
	SNK_API std::vector<std::pair<ds::bbox, std::uint32_t>> built_geometry_bounds(const geometry_desc& geometry);
}
