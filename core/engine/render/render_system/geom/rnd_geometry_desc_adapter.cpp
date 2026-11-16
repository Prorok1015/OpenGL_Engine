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
	}
	return entry;
}

void rnd::geometry_desc_adapter::load_binary(const ::res::tag& tag, rnd::geometry_desc& geometry) const
{
	const auto ranges = detail::binary_ranges_of(geometry);
	if (!ranges) {
		return;
	}
	const auto refuse = [&tag](std::string_view path, const std::string& reason) {
		throw ::res::load_error(::res::diagnostic_kind::load, path, std::format("geometry '{}': {}: {}", tag.view(), path, reason));
	};

	// Asked for in this order; `paths` names each in the desc.
	std::vector<::res::binary_range> asked;
	std::vector<std::string_view> paths;
	if (ranges->vertices) {
		const auto layout = ::res::parse_binary_layout(ranges->vertices->layout);
		if (!layout || !vertex_layout_fits(geometry.layout, *layout)) {
			refuse("/vertices/layout", std::format("'{}' does not fit the {}-byte vertex of the layout (\"u8\", or \"f32xN\" "
				"over float attributes)", ranges->vertices->layout, geometry.layout.get_stride()));
		}
		asked.push_back(*ranges->vertices);
		paths.push_back("/vertices");
	}
	if (ranges->indices) {
		if (ranges->indices->layout != "u32") {
			refuse("/indices/layout", std::format("'{}' is not \"u32\"", ranges->indices->layout));
		}
		asked.push_back(*ranges->indices);
		paths.push_back("/indices");
	}

	::res::desc_binary_data data = ::res::desc_binary::read(m_resources, tag, asked, verify_hash_on_load);
	if (!data) {
		// The message names the range as "range N (": that is the path of the N-th range asked for.
		std::string_view path = paths.front();
		for (std::size_t i = 0; i < paths.size(); ++i) {
			if (data.message.find(std::format("range {} (", i)) != std::string::npos) {
				path = paths[i];
				break;
			}
		}
		throw ::res::load_error(::res::diagnostic_kind::load, path, std::format("geometry '{}': {}", tag.view(), data.message));
	}

	static_assert(sizeof(unsigned int) == sizeof(std::uint32_t), "indices are u32 in the binary file");
	std::size_t at = 0;
	if (ranges->vertices && !::res::copy_binary_values(data.ranges[at++], geometry.vertices)) {
		refuse("/vertices", "the range is not whole bytes");
	}
	if (ranges->indices && !::res::copy_binary_values(data.ranges[at++], geometry.indices)) {
		refuse("/indices", "the range is not whole u32 values");
	}

	detail::check_geometry_data(tag, geometry);
	geometry.bounds.clear();
	geometry.rtree = {};
	if (ranges->wants_bounds) {
		detail::rebuild_bounds(geometry);
	}
	detail::set_binary_loaded(geometry);
}

std::vector<std::byte> rnd::geometry_desc_adapter::serialize(const ::res::tag& tag,
	const std::shared_ptr<::res::resource_entry>& resource) const
{
	return m_descs.serialize(tag, resource);
}
