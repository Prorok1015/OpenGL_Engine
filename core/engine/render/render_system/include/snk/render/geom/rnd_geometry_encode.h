#pragma once
#include <snk/common/ds/ds_bbox.hpp>
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_buffer_layout.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_tag.h>
#include <boost/json/object.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pure half of the geometry writer (EPIC-79, #1068): a geometry desc and its binary file encoded from data held
// anywhere, nothing read or stored. In snk_model_codec (#1077), which the model decoder links without the engine; the
// writer and the readers (rnd_geometry_files.h) stay in render.
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

	// The data of a geometry without a geometry_desc around it (geometry_view_of in rnd_geometry_files.h makes one of a
	// desc): nothing is copied, what it points at must outlive it.
	struct geometry_view
	{
		const driver::BufferLayout& layout;
		std::span<const std::byte> vertices;
		std::span<const unsigned int> indices;
		std::span<const std::pair<ds::bbox, std::uint32_t>> bounds;
	};

	// Why `geometry` cannot be written as `file` with its binary file; empty when it can. The loader's checks
	// (detail::geometry_data_problem: index past the vertices, a position or uv not finite, bytes off the stride, a partial
	// triangle) and: a topology other than triangles, no vertices, no indices (drawing is indexed), `file` not
	// <name>.<ext>.desc.
	SNK_API std::string geometry_write_problem(const res::tag& file, const geometry_view& geometry,
		const geometry_file_options& options = {});

	// The pair for `geometry` named `file`, nothing written; refused (`error`) as geometry_write_problem says. Vertices
	// are "f32xN" when every attribute is float, else "u8"; indices "u32".
	SNK_API geometry_files encode_geometry_files(const res::tag& file, const geometry_view& geometry,
		const geometry_file_options& options = {});

	// The arrays form of a geometry desc (the form before EPIC-79, kept for lines and points), what geometry_desc::serialize
	// writes: "__type", "layout", "indices", "vertices", "bounds", each set in `out` in that order. Throws std::runtime_error,
	// naming `file`, for a layout without vertices: an empty mesh is never written.
	SNK_API void write_geometry_arrays(const res::tag& file, const geometry_view& geometry, boost::json::object& out);

	namespace detail
	{
		// Why the loader refuses the data of `geometry` (`file` names it): `json_path` is where, `message` the whole text.
		struct geometry_data_refusal
		{
			std::string json_path;
			std::string message;
		};

		// `triangles`: the whole check of data for a binary file: a stride of 0, vertex bytes off the stride, an index count
		// off 3, an index past the vertices, no float position, a position or uv that is not finite. Else what holds for
		// arrays of any topology the loader takes. nullopt when the data passes.
		SNK_API std::optional<geometry_data_refusal> geometry_data_problem(const res::tag& file, const geometry_view& geometry,
			bool triangles);

		// The binary file's chunks over the data of a geometry, which must outlive them: vertices, then indices. Not copied:
		// the vertex chunk's layout points into `vertex_layout`.
		struct geometry_chunks
		{
			geometry_chunks() = default;
			geometry_chunks(const geometry_chunks&) = delete;
			geometry_chunks& operator=(const geometry_chunks&) = delete;

			std::string vertex_layout;
			std::array<res::binary_chunk, 2> chunks{};
		};

		// Fills `out` for `geometry`.
		SNK_API void fill_geometry_chunks(const geometry_view& geometry, geometry_chunks& out);

		// The desc of a pair: `ranges` are those of the chunks, vertices then indices. Compact JSON.
		SNK_API std::vector<std::byte> geometry_desc_bytes(const geometry_view& geometry, const geometry_file_options& options,
			std::span<const res::binary_range> ranges);
	}
}
