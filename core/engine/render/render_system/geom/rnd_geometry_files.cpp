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

	// Why `geometry` cannot be written as `file`; empty when it can. The loader's checks, so no writer makes a file it refuses.
	std::string write_problem(const res::tag& file, const rnd::geometry_desc& geometry)
	{
		if (!res::desc_binary::binary_tag(file)) {
			return std::format("'{}' is not named <name>.<ext>.desc: a geometry needs a binary file beside it", file.view());
		}
		if (geometry.vertices.empty()) {
			return std::format("geometry '{}' has no vertices: an empty mesh is not written", file.view());
		}
		try {
			rnd::detail::check_geometry_data(file, geometry);
		} catch (const std::exception& e) {
			return e.what();
		}
		return {};
	}

	std::span<const std::byte> index_bytes(const rnd::geometry_desc& geometry)
	{
		static_assert(sizeof(unsigned int) == sizeof(std::uint32_t), "indices are u32 in the binary file");
		static_assert(std::endian::native == std::endian::little, "the binary file is little-endian");
		return std::as_bytes(std::span<const unsigned int>(geometry.indices));
	}

	bool is_writers_key(std::string_view key)
	{
		return key == "__type" || key == "__version" || key == "layout" || key == "vertices" || key == "indices" || key == "bounds";
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
}

rnd::geometry_files rnd::encode_geometry_files(const res::tag& file, const geometry_desc& geometry,
	const geometry_file_options& options)
{
	PROFILE_SCOPE("geometry files encode");
	geometry_files out;
	out.error = write_problem(file, geometry);
	if (!out.error.empty()) {
		return out;
	}
	const std::string vertex_layout = vertex_layout_of(geometry.layout);
	const std::array<res::binary_chunk, 2> chunks{ {
		{ std::span<const std::byte>(geometry.vertices), vertex_layout },
		{ index_bytes(geometry), "u32" },
	} };
	std::vector<res::binary_range> ranges;
	auto binary = res::desc_binary::encode(chunks, ranges, out.error);
	if (!binary) {
		return out;
	}
	out.desc = desc_bytes(geometry, options, ranges);
	out.binary = std::move(*binary);
	return out;
}

std::string rnd::write_geometry_files(res::resource_system& system, const res::tag& file, const geometry_desc& geometry,
	const geometry_file_options& options)
{
	PROFILE_SCOPE("geometry files write");
	if (std::string problem = write_problem(file, geometry); !problem.empty()) {
		return problem;
	}
	const std::string vertex_layout = vertex_layout_of(geometry.layout);
	const std::array<res::binary_chunk, 2> chunks{ {
		{ std::span<const std::byte>(geometry.vertices), vertex_layout },
		{ index_bytes(geometry), "u32" },
	} };
	std::string desc_error;
	res::desc_binary_written written;
	try {
		written = res::desc_binary::save(system, file, chunks, [&](std::span<const res::binary_range> ranges) {
			if (!system.store_sync(file, desc_bytes(geometry, options, ranges))) {
				desc_error = std::format("'{}' could not be written (see the log); its old pair stays readable", file.view());
				return false;
			}
			return true;
		});
	} catch (const std::logic_error& e) {
		// A tag outside the writes the resource system allows.
		return std::format("'{}' may not be written: {}", file.view(), e.what());
	}
	if (!written) {
		return desc_error.empty() ? written.error : desc_error;
	}
	// A store does not evict the cache (the resolver reports its own writes to subscribers only): as every desc save.
	system.signal_changed(*res::desc_binary::binary_tag(file));
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

bool rnd::holds_geometry_arrays(const boost::json::object& desc)
{
	const boost::json::value* vertices = desc.if_contains("vertices");
	return vertices != nullptr && vertices->is_array();
}
