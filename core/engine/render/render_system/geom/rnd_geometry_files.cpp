#include <snk/render/geom/rnd_geometry_files.h>
#include "rnd_geometry_binary.h"
#include <snk/common/eng_profiler.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_system.h>
#include <boost/json/array.hpp>
#include <boost/json/serialize.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace
{
	bool is_float_type(rnd::driver::SHADER_DATA_TYPE type)
	{
		using rnd::driver::SHADER_DATA_TYPE;
		return type == SHADER_DATA_TYPE::FLOAT || type == SHADER_DATA_TYPE::VEC2_F || type == SHADER_DATA_TYPE::VEC3_F
			|| type == SHADER_DATA_TYPE::VEC4_F;
	}

	// The element is the whole vertex when every attribute is a float (what the loader accepts as "f32xN"), else bytes.
	std::string vertex_layout_of(const rnd::driver::BufferLayout& layout)
	{
		const std::size_t stride = layout.get_stride();
		if (stride == 0 || stride % sizeof(float) != 0 || stride / sizeof(float) > 32) {
			return "u8";
		}
		for (const auto& element : layout) {
			if (!is_float_type(element.Type)) {
				return "u8";
			}
		}
		return std::format("f32x{}", stride / sizeof(float));
	}

	std::span<const std::byte> index_bytes(const rnd::geometry_desc& geometry)
	{
		static_assert(sizeof(unsigned int) == sizeof(std::uint32_t), "indices are u32 in the binary file");
		static_assert(std::endian::native == std::endian::little, "the binary file is little-endian");
		return std::as_bytes(std::span<const unsigned int>(geometry.indices));
	}

	bool is_writers_key(std::string_view key)
	{
		return key == "__type" || key == "__version" || key == "layout" || key == "vertices" || key == "indices" || key == "bounds"
			|| key == rnd::geometry_topology_key;
	}

	// Ranges in the order of the chunks: vertices, then indices.
	std::vector<std::byte> desc_bytes(const rnd::geometry_desc& geometry, const rnd::geometry_file_options& options,
		std::span<const res::binary_range> ranges)
	{
		json::object out;
		out["__type"] = "geometry_desc";
		out["__version"] = rnd::geometry_binary_version;
		for (const auto& member : options.header) {
			if (!is_writers_key(member.key())) {
				out[member.key()] = member.value();
			}
		}
		out["layout"] = rnd::detail::layout_json(geometry.layout);
		out["vertices"] = res::to_json(ranges[0]);
		out["indices"] = res::to_json(ranges[1]);
		if (options.bounds) {
			out["bounds"] = json::array{};
		}
		const std::string text = json::serialize(out);
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		return bytes;
	}

	// The binary file's chunks over the data of `geometry`, which must outlive them.
	struct geometry_chunks
	{
		explicit geometry_chunks(const rnd::geometry_desc& geometry)
			: vertex_layout(vertex_layout_of(geometry.layout))
			, chunks{ {
				{ std::span<const std::byte>(geometry.vertices), vertex_layout },
				{ index_bytes(geometry), "u32" },
			} }
		{
		}
		geometry_chunks(const geometry_chunks&) = delete;
		geometry_chunks& operator=(const geometry_chunks&) = delete;

		std::string vertex_layout;
		std::array<res::binary_chunk, 2> chunks;
	};

	// The desc as it is before a write, under the save lock: what a failed commit puts back. nullopt when there is none.
	std::optional<std::vector<std::byte>> previous_desc_of(res::resource_system& system, const res::tag& file)
	{
		if (!system.exists(file)) {
			return std::nullopt;
		}
		const auto current = system.fetch_data(file);
		if (!current) {
			return std::nullopt;
		}
		const std::vector<std::byte>& bytes = current->get();
		if (current->has_error()) {
			return std::nullopt;
		}
		return bytes;
	}
}

std::string_view rnd::topology_name(geometry_topology topology)
{
	switch (topology) {
	case geometry_topology::triangles: return "triangles";
	case geometry_topology::lines: return "lines";
	case geometry_topology::points: return "points";
	case geometry_topology::mixed: return "mixed";
	}
	return "mixed";
}

rnd::geometry_topology rnd::topology_of(const boost::json::object& desc)
{
	const boost::json::value* named = desc.if_contains(geometry_topology_key);
	if (named == nullptr) {
		return geometry_topology::triangles;
	}
	if (named->is_string()) {
		for (const auto topology : { geometry_topology::triangles, geometry_topology::lines, geometry_topology::points }) {
			if (named->as_string() == topology_name(topology)) {
				return topology;
			}
		}
	}
	return geometry_topology::mixed;
}

std::string rnd::geometry_write_problem(const res::tag& file, const geometry_desc& geometry, const geometry_file_options& options)
{
	if (!res::desc_binary::binary_tag(file)) {
		return std::format("'{}' is not named <name>.<ext>.desc: a geometry needs a binary file beside it", file.view());
	}
	if (options.topology != geometry_topology::triangles) {
		return std::format("geometry '{}' holds {}, not triangles: only triangles are written with a binary file", file.view(),
			topology_name(options.topology));
	}
	if (geometry.vertices.empty()) {
		return std::format("geometry '{}' has no vertices: an empty mesh is not written", file.view());
	}
	if (geometry.indices.empty()) {
		return std::format("geometry '{}': /indices: there are no indices, a mesh is drawn by its indices only", file.view());
	}
	try {
		detail::check_geometry_data(file, geometry);
	} catch (const std::exception& e) {
		return e.what();
	}
	return {};
}

rnd::geometry_files rnd::encode_geometry_files(const res::tag& file, const geometry_desc& geometry,
	const geometry_file_options& options)
{
	PROFILE_SCOPE("geometry files encode");
	geometry_files out;
	out.error = geometry_write_problem(file, geometry, options);
	if (!out.error.empty()) {
		return out;
	}
	const geometry_chunks chunks(geometry);
	std::vector<res::binary_range> ranges;
	auto binary = res::desc_binary::encode(chunks.chunks, ranges, out.error);
	if (!binary) {
		return out;
	}
	out.desc = desc_bytes(geometry, options, ranges);
	out.binary = std::move(*binary);
	return out;
}

std::string rnd::write_geometry_files(res::resource_system& system, const res::tag& file, const geometry_desc& geometry,
	const geometry_file_options& options, std::vector<res::tag>* left)
{
	PROFILE_SCOPE("geometry files write");
	if (std::string problem = geometry_write_problem(file, geometry, options); !problem.empty()) {
		return problem;
	}
	const geometry_chunks chunks(geometry);
	std::string desc_error;
	std::optional<std::vector<std::byte>> previous_desc;
	std::vector<std::byte> written_desc;
	try {
		// Held across save() (it nests) and the take-back of a failed commit: no other save of the pair comes between.
		const res::desc_binary::save_lock lock(file);
		const res::desc_binary_written written = res::desc_binary::save(system, file, chunks.chunks,
			[&](std::span<const res::binary_range> ranges) {
				previous_desc = previous_desc_of(system, file);
				written_desc = desc_bytes(geometry, options, ranges);
				if (!system.store_sync(file, written_desc)) {
					desc_error = std::format("'{}' could not be written (see the log); its old pair stays readable", file.view());
					return false;
				}
				return true;
			});
		const auto staged = *res::desc_binary::staged_tag(file);
		switch (written.failed_step) {
		case res::desc_binary_save_step::none:
			break;
		case res::desc_binary_save_step::staging:
			return written.error;
		case res::desc_binary_save_step::desc:
			// No desc names the staged file: it is stale, the caller may take it away.
			if (left) left->push_back(staged);
			return desc_error.empty() ? written.error : desc_error;
		case res::desc_binary_save_step::commit:
			if (const res::file_operation back = res::desc_binary::take_back(system, lock, file, written, written_desc, previous_desc);
				!back) {
				// The desc stays and needs the staged file: both go back together or not at all.
				if (left) {
					left->push_back(staged);
					left->push_back(file);
				}
				return std::format("{}; taking it back failed too, the desc and its staged file stay: {}", written.error, back.error);
			}
			system.signal_changed(file);
			return std::format("{}; {}", written.error, previous_desc ? "the previous desc was put back" : "the desc was removed");
		}
	} catch (const std::logic_error& e) {
		// A tag outside the writes the resource system allows.
		return std::format("'{}' may not be written: {}", file.view(), e.what());
	}
	// A store does not evict the cache (the resolver reports its own writes to subscribers only). The desc depends on its
	// binary file, so one signal reaches what was built from either.
	system.signal_changed(file);
	return {};
}

std::string rnd::read_geometry_file(res::resource_system& system, const res::tag& file, const boost::json::object& desc,
	geometry_desc& out)
{
	PROFILE_SCOPE("geometry file read");
	try {
		detail::read_geometry_json(out, desc);
		if (detail::binary_state_of(out) == detail::geometry_binary_state::not_loaded) {
			detail::load_geometry_binary(system, file, out, true);
		}
	} catch (const res::load_error& e) {
		return e.what();
	} catch (const std::exception& e) {
		return std::format("geometry '{}': {}", file.view(), e.what());
	}
	return {};
}

std::string rnd::decode_geometry_files(const res::tag& file, const boost::json::object& desc, std::span<const std::byte> binary,
	geometry_desc& out)
{
	PROFILE_SCOPE("geometry file decode");
	try {
		detail::read_geometry_json(out, desc);
		if (detail::binary_state_of(out) == detail::geometry_binary_state::not_loaded) {
			detail::decode_geometry_binary(file, binary, out);
		} else if (!binary.empty()) {
			return std::format("geometry '{}' holds its data as arrays, but a binary file came with it", file.view());
		} else {
			detail::check_geometry_arrays(file, out);
		}
	} catch (const res::load_error& e) {
		return e.what();
	} catch (const std::exception& e) {
		return std::format("geometry '{}': {}", file.view(), e.what());
	}
	return {};
}

bool rnd::holds_geometry_arrays(const boost::json::object& desc)
{
	const boost::json::value* vertices = desc.if_contains("vertices");
	return vertices != nullptr && vertices->is_array();
}
