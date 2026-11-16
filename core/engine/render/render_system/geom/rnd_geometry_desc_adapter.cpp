#include "rnd_geometry_desc_adapter.h"
#include "rnd_geometry_binary.h"
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/resources/res_resource_binary.h>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	// Whether the text gives `key` an object somewhere: `"key"`, then `:` and `{` past white space. A match inside a
	// string only adds a harmless dependency.
	bool names_an_object(std::string_view text, std::string_view key)
	{
		const std::string quoted = std::format("\"{}\"", key);
		const auto skip_space = [&text](std::size_t at) {
			while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
				++at;
			}
			return at;
		};
		for (std::size_t at = text.find(quoted); at != std::string_view::npos; at = text.find(quoted, at + 1)) {
			std::size_t next = skip_space(at + quoted.size());
			if (next < text.size() && text[next] == ':') {
				next = skip_space(next + 1);
				if (next < text.size() && text[next] == '{') {
					return true;
				}
			}
		}
		return false;
	}

	// The binary file is read by deserialize (res::desc_binary::read, from the desc's layer); the dependency only puts
	// the edge into the graph, so it settles at once and loads nothing.
	std::shared_ptr<core::res::res_resource_block> watch_only(::res::resource_system&, const ::res::tag&)
	{
		auto block = std::make_shared<core::res::res_resource_block>();
		block->set_ready(nullptr);
		return block;
	}

	// The vertex range is bytes, or floats when every attribute is one: then its element is the whole vertex.
	bool vertex_layout_fits(const rnd::driver::BufferLayout& layout, const res::binary_layout& binary)
	{
		using rnd::driver::SHADER_DATA_TYPE;
		if (binary.scalar == res::binary_scalar::u8 && binary.count == 1) {
			return true;
		}
		if (binary.scalar != res::binary_scalar::f32 || binary.element_bytes() != layout.get_stride()) {
			return false;
		}
		for (const auto& element : layout) {
			const bool is_float = element.Type == SHADER_DATA_TYPE::FLOAT || element.Type == SHADER_DATA_TYPE::VEC2_F
				|| element.Type == SHADER_DATA_TYPE::VEC3_F || element.Type == SHADER_DATA_TYPE::VEC4_F;
			if (!is_float) {
				return false;
			}
		}
		return true;
	}
}

rnd::geometry_desc_adapter::geometry_desc_adapter(::res::resource_system& res_system, ::desc::desc_system& desc_system)
	: m_resources(res_system)
	, m_descs(res_system, desc_system)
{
}

std::vector<::res::res_dep> rnd::geometry_desc_adapter::collect_dependencies(const ::res::tag& tag,
	const std::vector<std::byte>& raw_data) const
{
	auto deps = m_descs.collect_dependencies(tag, raw_data);
	const std::string_view text(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
	if (names_an_object(text, "vertices") || names_an_object(text, "indices")) {
		if (const auto binary = ::res::desc_binary::binary_tag(tag)) {
			deps.push_back(::res::res_dep{ *binary, ds::type_id::make<::res::binary_resource>(), &watch_only, {}, "/vertices" });
		}
	}
	return deps;
}

std::shared_ptr<::res::resource_entry> rnd::geometry_desc_adapter::deserialize(const ::res::tag& tag,
	const std::vector<std::byte>& raw_data) const
{
	return deserialize(tag, raw_data, ::res::res_deps{});
}

std::shared_ptr<::res::resource_entry> rnd::geometry_desc_adapter::deserialize(const ::res::tag& tag,
	const std::vector<std::byte>& raw_data, const ::res::res_deps& deps) const
{
	auto entry = m_descs.deserialize(tag, raw_data, deps);
	const auto geometry = std::dynamic_pointer_cast<rnd::geometry_desc>(entry);
	if (!geometry) {
		const auto* loaded = dynamic_cast<const ::desc::desc_base*>(entry.get());
		const std::string type = loaded ? std::string(loaded->get_type()) : std::string("resource");
		throw ::res::load_error(::res::diagnostic_kind::unknown_type, "/__type",
			std::format("desc '{}' is a '{}', not a geometry_desc", tag.view(), type));
	}
	if (detail::binary_state_of(*geometry) == detail::geometry_binary_state::not_loaded) {
		load_binary(tag, *geometry);
	} else {
		// Arrays (the form before EPIC-79, and lines or points now): what holds for any topology, before the GPU gets them.
		detail::check_geometry_arrays(tag, *geometry);
	}
	return entry;
}

void rnd::geometry_desc_adapter::load_binary(const ::res::tag& tag, rnd::geometry_desc& geometry) const
{
	detail::load_geometry_binary(m_resources, tag, geometry, verify_hash_on_load);
}

namespace
{
	[[noreturn]] void refuse_binary(const ::res::tag& tag, std::string_view path, const std::string& reason)
	{
		throw ::res::load_error(::res::diagnostic_kind::load, path, std::format("geometry '{}': {}: {}", tag.view(), path, reason));
	}

	// The recorded ranges in the order they are asked for, `paths` naming each in the desc.
	struct asked_ranges
	{
		std::vector<::res::binary_range> ranges;
		std::vector<std::string_view> paths;
	};

	asked_ranges ask_ranges(const ::res::tag& tag, const rnd::geometry_desc& geometry, const rnd::detail::geometry_binary_ranges& ranges)
	{
		asked_ranges asked;
		if (ranges.vertices) {
			const auto layout = ::res::parse_binary_layout(ranges.vertices->layout);
			if (!layout || !vertex_layout_fits(geometry.layout, *layout)) {
				refuse_binary(tag, "/vertices/layout", std::format("'{}' does not fit the {}-byte vertex of the layout (\"u8\", or "
					"\"f32xN\" over float attributes)", ranges.vertices->layout, geometry.layout.get_stride()));
			}
			asked.ranges.push_back(*ranges.vertices);
			asked.paths.push_back("/vertices");
		}
		if (ranges.indices) {
			if (ranges.indices->layout != "u32") {
				refuse_binary(tag, "/indices/layout", std::format("'{}' is not \"u32\"", ranges.indices->layout));
			}
			asked.ranges.push_back(*ranges.indices);
			asked.paths.push_back("/indices");
		}
		return asked;
	}

	// What desc_binary read or parsed, checked and copied into `geometry` as the loader takes it.
	void take_ranges(const ::res::tag& tag, rnd::geometry_desc& geometry, const rnd::detail::geometry_binary_ranges& ranges,
		const asked_ranges& asked, const ::res::desc_binary_data& data)
	{
		if (!data) {
			// The message names the range as "range N (": that is the path of the N-th range asked for.
			std::string_view path = asked.paths.front();
			for (std::size_t i = 0; i < asked.paths.size(); ++i) {
				if (data.message.find(std::format("range {} (", i)) != std::string::npos) {
					path = asked.paths[i];
					break;
				}
			}
			throw ::res::load_error(::res::diagnostic_kind::load, path, std::format("geometry '{}': {}", tag.view(), data.message));
		}

		static_assert(sizeof(unsigned int) == sizeof(std::uint32_t), "indices are u32 in the binary file");
		std::size_t at = 0;
		if (ranges.vertices && !::res::copy_binary_values(data.ranges[at++], geometry.vertices)) {
			refuse_binary(tag, "/vertices", "the range is not whole bytes");
		}
		if (ranges.indices && !::res::copy_binary_values(data.ranges[at++], geometry.indices)) {
			refuse_binary(tag, "/indices", "the range is not whole u32 values");
		}

		rnd::detail::check_geometry_data(tag, geometry);
		geometry.bounds.clear();
		geometry.rtree = {};
		if (ranges.wants_bounds) {
			rnd::detail::rebuild_bounds(geometry);
		}
		rnd::detail::set_binary_loaded(geometry);
	}
}

void rnd::detail::load_geometry_binary(::res::resource_system& system, const ::res::tag& tag, rnd::geometry_desc& geometry,
	bool verify_hash)
{
	const auto ranges = detail::binary_ranges_of(geometry);
	if (!ranges) {
		return;
	}
	const asked_ranges asked = ask_ranges(tag, geometry, *ranges);
	take_ranges(tag, geometry, *ranges, asked, ::res::desc_binary::read(system, tag, asked.ranges, verify_hash));
}

void rnd::detail::decode_geometry_binary(const ::res::tag& tag, std::span<const std::byte> file, rnd::geometry_desc& geometry)
{
	const auto ranges = detail::binary_ranges_of(geometry);
	if (!ranges) {
		return;
	}
	const auto binary = ::res::desc_binary::binary_tag(tag);
	if (!binary) {
		refuse_binary(tag, "/vertices", "the desc is not named <name>.<ext>.desc: it has no binary file");
	}
	if (file.size() > ::res::desc_binary::max_file_bytes) {
		refuse_binary(tag, "/vertices", std::format("'{}' is {} bytes, more than the {} a binary file may have", binary->view(),
			file.size(), ::res::desc_binary::max_file_bytes));
	}
	const asked_ranges asked = ask_ranges(tag, geometry, *ranges);
	take_ranges(tag, geometry, *ranges, asked, ::res::desc_binary::parse(binary->view(), file, asked.ranges, true));
}

std::vector<std::byte> rnd::geometry_desc_adapter::serialize(const ::res::tag& tag,
	const std::shared_ptr<::res::resource_entry>&) const
{
	// Vertices and indices go to the binary file beside the desc, which a store of one file cannot write (EPIC-79).
	throw std::runtime_error(std::format("geometry '{}' is not written by a store of the resource: rnd::write_geometry_files "
		"writes the desc with its binary file", tag.view()));
}
