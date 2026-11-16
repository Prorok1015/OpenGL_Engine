#include <snk/render/geom/rnd_geometry_encode.h>
#include <snk/common/eng_profiler.h>
#include <boost/json/array.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_from.hpp>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>

namespace json = boost::json;

namespace
{
	std::string_view shader_data_type_to_string(rnd::driver::SHADER_DATA_TYPE type)
	{
		using namespace rnd::driver;
		switch (type)
		{
		case SHADER_DATA_TYPE::FLOAT:    return "float";
		case SHADER_DATA_TYPE::VEC2_F:   return "vec2";
		case SHADER_DATA_TYPE::VEC3_F:   return "vec3";
		case SHADER_DATA_TYPE::VEC4_F:   return "vec4";
		case SHADER_DATA_TYPE::MAT3_F:   return "mat3";
		case SHADER_DATA_TYPE::MAT4_F:   return "mat4";
		case SHADER_DATA_TYPE::INT:      return "int";
		case SHADER_DATA_TYPE::VEC2_I:   return "ivec2";
		case SHADER_DATA_TYPE::VEC3_I:   return "ivec3";
		case SHADER_DATA_TYPE::VEC4_I:   return "ivec4";
		case SHADER_DATA_TYPE::BOOL:     return "bool";
		case SHADER_DATA_TYPE::UNKNOWN:  break;
		}
		return "unknown";
	}
}

// What json::value_from makes of the parts of a geometry (moved here from rnd_geometry_desc.cpp with the writer of arrays).
namespace rnd::driver
{
	void tag_invoke(json::value_from_tag, json::value& jv, const rnd::driver::BufferElement& c)
	{
		jv = shader_data_type_to_string(c.Type);
	}

	void tag_invoke(json::value_from_tag, json::value& jv, const rnd::driver::BufferLayout& c)
	{
		json::object ly;
		for (auto& elem : c) {
			ly[elem.Name] = json::value_from(elem);
		}
		jv = ly;
	}
}

namespace std
{
	void tag_invoke(json::value_from_tag, json::value& jv, const std::byte& c)
	{
		uint64_t val = 0ull + (unsigned char)c;
		jv = val;
	}
}

namespace ds
{
	void tag_invoke(json::value_from_tag, json::value& jv, const ds::bbox& c)
	{
		json::array arr;
		arr.push_back({ {"x", c.min.x}, {"y", c.min.y}});
		arr.push_back({ {"x", c.max.x}, {"y", c.max.y} });
		jv = arr;
	}
}

namespace
{
	bool is_float_type(rnd::driver::SHADER_DATA_TYPE type)
	{
		using rnd::driver::SHADER_DATA_TYPE;
		return type == SHADER_DATA_TYPE::FLOAT || type == SHADER_DATA_TYPE::VEC2_F || type == SHADER_DATA_TYPE::VEC3_F
			|| type == SHADER_DATA_TYPE::VEC4_F;
	}

	const rnd::driver::BufferElement* element_named(const rnd::driver::BufferLayout& layout, std::string_view name)
	{
		for (const auto& element : layout) {
			if (element.Name == name) {
				return &element;
			}
		}
		return nullptr;
	}

	float float_at(std::span<const std::byte> vertices, std::size_t offset)
	{
		float value = 0.0f;
		std::memcpy(&value, vertices.data() + offset, sizeof(value));
		return value;
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

	std::span<const std::byte> index_bytes(const rnd::geometry_view& geometry)
	{
		static_assert(sizeof(unsigned int) == sizeof(std::uint32_t), "indices are u32 in the binary file");
		static_assert(std::endian::native == std::endian::little, "the binary file is little-endian");
		return std::as_bytes(geometry.indices);
	}

	bool is_writers_key(std::string_view key)
	{
		return key == "__type" || key == "__version" || key == "layout" || key == "vertices" || key == "indices" || key == "bounds"
			|| key == rnd::geometry_topology_key;
	}

	// The "layout" object of a desc.
	json::value layout_json(const rnd::driver::BufferLayout& layout)
	{
		return json::value_from(layout);
	}

	rnd::detail::geometry_data_refusal refusal(const res::tag& file, std::string_view path, const std::string& reason)
	{
		return { std::string(path), std::format("geometry '{}': {}: {}", file.view(), path, reason) };
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

std::optional<rnd::detail::geometry_data_refusal> rnd::detail::geometry_data_problem(const res::tag& file,
	const geometry_view& geometry, bool triangles)
{
	const std::size_t stride = geometry.layout.get_stride();
	if (stride == 0) {
		return refusal(file, "/layout", "the vertex layout is empty");
	}
	if (geometry.vertices.size() % stride != 0) {
		return refusal(file, "/vertices", std::format("{} bytes are not a whole number of {}-byte vertices", geometry.vertices.size(), stride));
	}
	const std::size_t vertex_count = geometry.vertices.size() / stride;
	if (triangles && geometry.indices.size() % 3 != 0) {
		return refusal(file, "/indices", std::format("{} indices are not whole triangles", geometry.indices.size()));
	}
	for (std::size_t i = 0; i < geometry.indices.size(); ++i) {
		if (geometry.indices[i] >= vertex_count) {
			return refusal(file, "/indices", std::format("index {} is {}, past the {} vertices", i, geometry.indices[i], vertex_count));
		}
	}

	const auto* position = element_named(geometry.layout, "position");
	if (triangles && (position == nullptr || !is_float_type(position->Type))) {
		return refusal(file, "/layout/position", "the layout has no float 'position'");
	}
	// The CPU reads these two: positions for the scene, uv for the bounds.
	for (const auto* element : { position, element_named(geometry.layout, "uv") }) {
		if (element == nullptr || !is_float_type(element->Type)) {
			continue;
		}
		const std::size_t components = element->Size / sizeof(float);
		for (std::size_t v = 0; v < vertex_count; ++v) {
			for (std::size_t c = 0; c < components; ++c) {
				const float value = float_at(geometry.vertices, v * stride + element->Offset + c * sizeof(float));
				if (!std::isfinite(value)) {
					return refusal(file, "/vertices", std::format("vertex {} has a '{}' that is not finite", v, element->Name));
				}
			}
		}
	}
	return std::nullopt;
}

void rnd::detail::fill_geometry_chunks(const geometry_view& geometry, geometry_chunks& out)
{
	out.vertex_layout = vertex_layout_of(geometry.layout);
	out.chunks = { {
		{ geometry.vertices, out.vertex_layout },
		{ index_bytes(geometry), "u32" },
	} };
}

std::vector<std::byte> rnd::detail::geometry_desc_bytes(const geometry_view& geometry, const geometry_file_options& options,
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
	out["layout"] = layout_json(geometry.layout);
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

std::string rnd::geometry_write_problem(const res::tag& file, const geometry_view& geometry, const geometry_file_options& options)
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
	if (const auto refused = detail::geometry_data_problem(file, geometry, true)) {
		return refused->message;
	}
	return {};
}

rnd::geometry_files rnd::encode_geometry_files(const res::tag& file, const geometry_view& geometry,
	const geometry_file_options& options)
{
	PROFILE_SCOPE("geometry files encode");
	geometry_files out;
	out.error = geometry_write_problem(file, geometry, options);
	if (!out.error.empty()) {
		return out;
	}
	detail::geometry_chunks chunks;
	detail::fill_geometry_chunks(geometry, chunks);
	std::vector<res::binary_range> ranges;
	auto binary = res::desc_binary::encode(chunks.chunks, ranges, out.error);
	if (!binary) {
		return out;
	}
	out.desc = detail::geometry_desc_bytes(geometry, options, ranges);
	out.binary = std::move(*binary);
	return out;
}

void rnd::write_geometry_arrays(const res::tag& file, const geometry_view& geometry, json::object& out)
{
	// A layout without vertices is a mesh whose data is elsewhere or lost, never written empty.
	if (geometry.vertices.empty() && (geometry.layout.begin() != geometry.layout.end() || geometry.layout.get_stride() != 0)) {
		throw std::runtime_error(std::format("geometry '{}' has a vertex layout but no vertices: an empty mesh is not written",
			file.view()));
	}
	// Element by element as json::value_from converts a vector: the same values as before the view (#1077).
	const auto array_of = [](const auto& items) {
		json::array array;
		array.reserve(items.size());
		for (const auto& item : items) {
			array.emplace_back(json::value_from(item));
		}
		return array;
	};
	out["__type"] = "geometry_desc";
	out["layout"] = layout_json(geometry.layout);
	out["indices"] = array_of(geometry.indices);
	out["vertices"] = array_of(geometry.vertices);
	out["bounds"] = array_of(geometry.bounds);
}
