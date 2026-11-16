#include <snk/render/geom/rnd_geometry_desc.h>
#include "rnd_geometry_binary.h"
#include <snk/desc/desc_system.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/common/eng_profiler.h>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

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

void shader_data_type_from_string(const std::string_view& type, rnd::driver::SHADER_DATA_TYPE& out)
{
	using namespace rnd::driver;
	if (type == "float")    out = SHADER_DATA_TYPE::FLOAT;
	if (type == "vec2")     out = SHADER_DATA_TYPE::VEC2_F;
	if (type == "vec3")     out = SHADER_DATA_TYPE::VEC3_F;
	if (type == "vec4")     out = SHADER_DATA_TYPE::VEC4_F;
	if (type == "mat3")     out = SHADER_DATA_TYPE::MAT3_F;
	if (type == "mat4")     out = SHADER_DATA_TYPE::MAT4_F;
	if (type == "int")      out = SHADER_DATA_TYPE::INT;
	if (type == "ivec2")    out = SHADER_DATA_TYPE::VEC2_I;
	if (type == "ivec3")    out = SHADER_DATA_TYPE::VEC3_I;
	if (type == "ivec4")    out = SHADER_DATA_TYPE::VEC4_I;
	if (type == "bool")     out = SHADER_DATA_TYPE::BOOL;
}

rnd::driver::SHADER_DATA_TYPE shader_data_type_from_string(const std::string_view& type)
{
	using namespace rnd::driver;
	if (type == "float")    return SHADER_DATA_TYPE::FLOAT;
	if (type == "vec2")     return SHADER_DATA_TYPE::VEC2_F;
	if (type == "vec3")     return SHADER_DATA_TYPE::VEC3_F;
	if (type == "vec4")     return SHADER_DATA_TYPE::VEC4_F;
	if (type == "mat3")     return SHADER_DATA_TYPE::MAT3_F;
	if (type == "mat4")     return SHADER_DATA_TYPE::MAT4_F;
	if (type == "int")      return SHADER_DATA_TYPE::INT;
	if (type == "ivec2")    return SHADER_DATA_TYPE::VEC2_I;
	if (type == "ivec3")    return SHADER_DATA_TYPE::VEC3_I;
	if (type == "ivec4")    return SHADER_DATA_TYPE::VEC4_I;
	if (type == "bool")     return SHADER_DATA_TYPE::BOOL;
	return SHADER_DATA_TYPE::UNKNOWN;
}


namespace rnd::driver {
	void tag_invoke(json::value_from_tag, json::value& jv, const rnd::driver::BufferElement& c)
	{
		jv = shader_data_type_to_string(c.Type);
	}

	void tag_invoke(json::value_from_tag t, json::value& jv, const rnd::driver::BufferLayout& c)
	{
		json::object ly;
		for (auto& elem : c) {
			ly[elem.Name] = json::value_from(elem);
		}
		jv = ly;
	}

}

namespace
{
	// From a file: a value that is no byte fails the load. It is never cut down to one (256 read as 0) and never
	// asserted on (#930).
	std::byte byte_of(const json::value& jv)
	{
		if (jv.is_uint64() && jv.as_uint64() <= 255) {
			return std::byte(jv.as_uint64());
		}
		if (jv.is_int64() && jv.as_int64() >= 0 && jv.as_int64() <= 255) {
			return std::byte(jv.as_int64());
		}
		throw res::load_error(res::diagnostic_kind::load, {}, "a byte of geometry data is not an integer from 0 to 255");
	}
}

namespace std
{
	void tag_invoke(json::value_from_tag, json::value& jv, const std::byte& c)
	{
		uint64_t val = 0ull + (unsigned char)c;
		jv = val;
	}

	std::byte tag_invoke(json::value_to_tag<std::byte>, const json::value& jv)
	{
		return byte_of(jv);
	}
}

namespace ds {
	void tag_invoke(json::value_from_tag, json::value& jv, const ds::bbox& c)
	{
		json::array arr;
		arr.push_back({ {"x", c.min.x}, {"y", c.min.y}});
		arr.push_back({ {"x", c.max.x}, {"y", c.max.y} });
		jv = arr;
	}

	ds::bbox tag_invoke(json::value_to_tag<ds::bbox>, const json::value& jv)
	{
		const json::array* arr = jv.if_array();
		const auto point = [](const json::value& v, const char* key) -> const json::value* {
			const json::object* object = v.if_object();
			const json::value* found = object ? object->if_contains(key) : nullptr;
			return found && found->is_number() ? found : nullptr;
		};
		const bool fits = arr && arr->size() == 2 && point((*arr)[0], "x") && point((*arr)[0], "y") && point((*arr)[1], "x")
			&& point((*arr)[1], "y");
		if (!fits) {
			throw res::load_error(res::diagnostic_kind::load, {}, "a bounding box is not an array of two points {x, y}");
		}
		ds::bbox box;
		box.min.x = json::value_to<float>(*point((*arr)[0], "x"));
		box.min.y = json::value_to<float>(*point((*arr)[0], "y"));
		box.max.x = json::value_to<float>(*point((*arr)[1], "x"));
		box.max.y = json::value_to<float>(*point((*arr)[1], "y"));
		return box;
	}
}

namespace
{
	struct binary_entry
	{
		rnd::detail::geometry_binary_state state = rnd::detail::geometry_binary_state::none;
		rnd::detail::geometry_binary_ranges ranges;
	};

	// By address: geometry_desc has no member for it. Never destroyed, so a geometry freed at exit still finds it.
	struct binary_registry
	{
		std::mutex mutex;
		std::unordered_map<const rnd::geometry_desc*, binary_entry> entries;
	};

	binary_registry& registry()
	{
		static binary_registry* const made = new binary_registry;
		return *made;
	}

	void copy_binary(const rnd::geometry_desc& from, const rnd::geometry_desc& to)
	{
		binary_registry& reg = registry();
		std::lock_guard lock(reg.mutex);
		const auto found = reg.entries.find(&from);
		if (found == reg.entries.end()) {
			reg.entries.erase(&to);
			return;
		}
		binary_entry copy = found->second;
		reg.entries.insert_or_assign(&to, std::move(copy));
	}

	// A range object: recorded for the adapter. Anything else is not a range (an array, read as before).
	std::optional<res::binary_range> range_at(const json::value& value, std::string_view path)
	{
		if (!value.is_object()) {
			return std::nullopt;
		}
		std::string error;
		auto range = res::binary_range_from_json(value, error);
		if (!range) {
			throw res::load_error(res::diagnostic_kind::load, path, std::format("{}: {}", path, error));
		}
		return range;
	}

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

	[[noreturn]] void refuse_data(const res::tag& file, std::string_view path, const std::string& reason)
	{
		throw res::load_error(res::diagnostic_kind::load, path, std::format("geometry '{}': {}: {}", file.view(), path, reason));
	}

	float float_at(const std::vector<std::byte>& vertices, std::size_t offset)
	{
		float value = 0.0f;
		std::memcpy(&value, vertices.data() + offset, sizeof(value));
		return value;
	}
}

rnd::detail::geometry_binary_state rnd::detail::binary_state_of(const geometry_desc& geometry)
{
	binary_registry& reg = registry();
	std::lock_guard lock(reg.mutex);
	const auto found = reg.entries.find(&geometry);
	return found == reg.entries.end() ? geometry_binary_state::none : found->second.state;
}

std::optional<rnd::detail::geometry_binary_ranges> rnd::detail::binary_ranges_of(const geometry_desc& geometry)
{
	binary_registry& reg = registry();
	std::lock_guard lock(reg.mutex);
	const auto found = reg.entries.find(&geometry);
	if (found == reg.entries.end() || found->second.state != geometry_binary_state::not_loaded) {
		return std::nullopt;
	}
	return found->second.ranges;
}

void rnd::detail::set_binary_ranges(const geometry_desc& geometry, geometry_binary_ranges ranges)
{
	binary_registry& reg = registry();
	std::lock_guard lock(reg.mutex);
	reg.entries.insert_or_assign(&geometry, binary_entry{ geometry_binary_state::not_loaded, std::move(ranges) });
}

void rnd::detail::set_binary_loaded(const geometry_desc& geometry)
{
	binary_registry& reg = registry();
	std::lock_guard lock(reg.mutex);
	reg.entries[&geometry].state = geometry_binary_state::loaded;
}

void rnd::detail::forget_binary(const geometry_desc& geometry)
{
	binary_registry& reg = registry();
	std::lock_guard lock(reg.mutex);
	reg.entries.erase(&geometry);
}

namespace
{
	// `triangles`: the whole check of data for a binary file; else what holds for arrays of any topology the loader takes.
	void check_data(const res::tag& file, const rnd::geometry_desc& geometry, bool triangles)
	{
		const std::size_t stride = geometry.layout.get_stride();
		if (stride == 0) {
			refuse_data(file, "/layout", "the vertex layout is empty");
		}
		if (geometry.vertices.size() % stride != 0) {
			refuse_data(file, "/vertices", std::format("{} bytes are not a whole number of {}-byte vertices", geometry.vertices.size(), stride));
		}
		const std::size_t vertex_count = geometry.vertices.size() / stride;
		if (triangles && geometry.indices.size() % 3 != 0) {
			refuse_data(file, "/indices", std::format("{} indices are not whole triangles", geometry.indices.size()));
		}
		for (std::size_t i = 0; i < geometry.indices.size(); ++i) {
			if (geometry.indices[i] >= vertex_count) {
				refuse_data(file, "/indices", std::format("index {} is {}, past the {} vertices", i, geometry.indices[i], vertex_count));
			}
		}

		const auto* position = element_named(geometry.layout, "position");
		if (triangles && (position == nullptr || !is_float_type(position->Type))) {
			refuse_data(file, "/layout/position", "the layout has no float 'position'");
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
						refuse_data(file, "/vertices", std::format("vertex {} has a '{}' that is not finite", v, element->Name));
					}
				}
			}
		}
	}
}

void rnd::detail::check_geometry_data(const res::tag& file, const geometry_desc& geometry)
{
	check_data(file, geometry, true);
}

void rnd::detail::check_geometry_arrays(const res::tag& file, const geometry_desc& geometry)
{
	// A desc without indices and without vertices (base_geometry.desc) has nothing the checks could hold against. Indices
	// with no vertex to index, or over a layout of no bytes, are refused by check_data.
	if (geometry.indices.empty() && (geometry.vertices.empty() || geometry.layout.get_stride() == 0)) {
		return;
	}
	check_data(file, geometry, false);
}

void rnd::detail::rebuild_bounds(geometry_desc& geometry)
{
	PROFILE_SCOPE("geometry_desc bounds rebuild");
	geometry.bounds.clear();
	geometry.rtree = {};
	const std::size_t stride = geometry.layout.get_stride();
	const auto* uv = element_named(geometry.layout, "uv");
	const bool has_uv = uv != nullptr && uv->Type == rnd::driver::SHADER_DATA_TYPE::VEC2_F && stride != 0;
	const auto uv_of = [&](unsigned int vertex) {
		if (!has_uv) {
			return ds::point2d{ 0.0f };
		}
		const std::size_t at = vertex * stride + uv->Offset;
		return ds::point2d{ float_at(geometry.vertices, at), float_at(geometry.vertices, at + sizeof(float)) };
	};
	geometry.bounds.reserve(geometry.indices.size() / 3);
	for (std::size_t i = 0; i + 2 < geometry.indices.size(); i += 3) {
		ds::bbox box;
		for (std::size_t k = 0; k < 3; ++k) {
			ds::expand(box, uv_of(geometry.indices[i + k]));
		}
		geometry.bounds.push_back({ box, static_cast<std::uint32_t>(geometry.bounds.size()) });
	}
	for (const auto& [box, triangle] : geometry.bounds) {
		geometry.rtree.insert(box, triangle);
	}
}

rnd::geometry_desc::geometry_desc()
{
	// The address may be one a geometry destroyed without this dtor (an inline one of an old module) left in the table.
	detail::forget_binary(*this);
}

rnd::geometry_desc::geometry_desc(const geometry_desc& other)
	: desc::desc_base(other)
	, layout(other.layout)
	, indices(other.indices)
	, vertices(other.vertices)
	, bounds(other.bounds)
	, rtree(other.rtree)
{
	copy_binary(other, *this);
}

rnd::geometry_desc& rnd::geometry_desc::operator=(const geometry_desc& other)
{
	if (this == &other) {
		return *this;
	}
	desc::desc_base::operator=(other);
	layout = other.layout;
	indices = other.indices;
	vertices = other.vertices;
	bounds = other.bounds;
	rtree = other.rtree;
	copy_binary(other, *this);
	return *this;
}

rnd::geometry_desc::~geometry_desc()
{
	detail::forget_binary(*this);
}

void rnd::geometry_desc::deserialize(desc::desc_system&, const json::object& resource)
{
	detail::read_geometry_json(*this, resource);
}

json::value rnd::detail::layout_json(const driver::BufferLayout& layout)
{
	return json::value_from(layout);
}

void rnd::detail::read_geometry_json(geometry_desc& geometry, const json::object& resource)
{
	auto& layout = geometry.layout;
	auto& indices = geometry.indices;
	auto& vertices = geometry.vertices;
	auto& bounds = geometry.bounds;
	auto& rtree = geometry.rtree;
	auto& json_layout = resource.at("layout");
	{
		std::vector<rnd::driver::BufferElement> elements;
		for (const auto& [name, type] : json_layout.as_object())
		{
			elements.push_back({ shader_data_type_from_string(type.as_string()), name });
		}

		layout = rnd::driver::BufferLayout{ elements };
	}

	detail::geometry_binary_ranges ranges;
	{
		auto& json_indices = resource.at("indices");
		ranges.indices = range_at(json_indices, "/indices");
		if (ranges.indices) {
			indices.clear();
		} else if (!json_indices.as_array().empty()) {
			indices = json::value_to<std::remove_cvref_t<decltype(indices)>>(json_indices);
		}
	}

	{
		const json::value& json_value = resource.at("vertices");
		ranges.vertices = range_at(json_value, "/vertices");
		// One byte a number: a model's vertices are millions of them, so not value_to element by element (#1022).
		if (ranges.vertices) {
			vertices.clear();
		} else if (const json::array& json_vertices = json_value.as_array(); !json_vertices.empty()) {
			std::vector<std::byte> read;
			read.reserve(json_vertices.size());
			for (const json::value& item : json_vertices) {
				read.push_back(byte_of(item));
			}
			vertices = std::move(read);
		}
	}

	if (ranges.vertices || ranges.indices) {
		// The data is the binary file's: stored bounds are not trusted, the adapter builds them from the checked data.
		ranges.wants_bounds = resource.contains("bounds");
		bounds.clear();
		rtree = {};
		detail::set_binary_ranges(geometry, std::move(ranges));
		return;
	}
	detail::forget_binary(geometry);

	if (resource.contains("bounds"))
	{
		auto& json_bounds = resource.at("bounds");
		bounds = json::value_to<std::remove_cvref_t<decltype(bounds)>>(json_bounds);

		PROFILE_SCOPE("geometry_desc rtree build");
		for(auto& [key, val] : bounds)
		{
			rtree.insert(key, val);
		}

		//rtree.build(bounds);
	}
}

void rnd::geometry_desc::serialize(json::object& resource) const
{
	// A geometry read from a binary file is never written as arrays nor as nothing: rnd::write_geometry_files writes it.
	switch (detail::binary_state_of(*this)) {
	case detail::geometry_binary_state::not_loaded:
		throw std::runtime_error(std::format("geometry '{}': its binary data was not loaded (it was not read as a geometry), "
			"an empty mesh is not written", get_tag().view()));
	case detail::geometry_binary_state::loaded:
		throw std::runtime_error(std::format("geometry '{}' was read from its binary file: it is not written back as arrays",
			get_tag().view()));
	case detail::geometry_binary_state::none:
		break;
	}
	// Whatever the table says: a layout without vertices is a mesh whose data is elsewhere or lost, never written empty.
	if (vertices.empty() && (layout.begin() != layout.end() || layout.get_stride() != 0)) {
		throw std::runtime_error(std::format("geometry '{}' has a vertex layout but no vertices: an empty mesh is not written",
			get_tag().view()));
	}
	resource["__type"] = "geometry_desc";
	resource["layout"] = json::value_from(layout);
	resource["indices"] = json::value_from(indices);
	resource["vertices"] = json::value_from(vertices);
	resource["bounds"] = json::value_from(bounds);
}
