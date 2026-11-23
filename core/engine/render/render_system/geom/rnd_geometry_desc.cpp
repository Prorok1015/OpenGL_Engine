#include <snk/render/geom/rnd_geometry_desc.h>
#include "rnd_geometry_binary.h"
#include <snk/desc/desc_system.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/common/eng_profiler.h>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
	std::byte tag_invoke(json::value_to_tag<std::byte>, const json::value& jv)
	{
		return byte_of(jv);
	}
}

namespace ds {
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

	const rnd::driver::BufferElement* element_named(const rnd::driver::BufferLayout& layout, std::string_view name)
	{
		for (const auto& element : layout) {
			if (element.Name == name) {
				return &element;
			}
		}
		return nullptr;
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
	return geometry.binary.state;
}

std::optional<rnd::detail::geometry_binary_ranges> rnd::detail::binary_ranges_of(const geometry_desc& geometry)
{
	if (geometry.binary.state != geometry_binary_state::not_loaded) {
		return std::nullopt;
	}
	return geometry.binary.ranges;
}

void rnd::detail::set_binary_ranges(geometry_desc& geometry, geometry_binary_ranges ranges)
{
	geometry.binary = geometry_binary{ geometry_binary_state::not_loaded, std::move(ranges) };
}

void rnd::detail::set_binary_loaded(geometry_desc& geometry)
{
	geometry.binary.state = geometry_binary_state::loaded;
}

void rnd::detail::forget_binary(geometry_desc& geometry)
{
	geometry.binary = {};
}

namespace
{
	// What geometry_data_problem refuses, thrown as the loader reports it.
	void check_data(const res::tag& file, const rnd::geometry_desc& geometry, bool triangles)
	{
		if (const auto refused = rnd::detail::geometry_data_problem(file, rnd::geometry_view_of(geometry), triangles)) {
			throw res::load_error(res::diagnostic_kind::load, refused->json_path, refused->message);
		}
	}
}

void rnd::detail::check_geometry_data(const res::tag& file, const geometry_desc& geometry)
{
	check_data(file, geometry, true);
}

void rnd::detail::check_geometry_arrays(const res::tag& file, const geometry_desc& geometry)
{
	// A desc without data (base_geometry.desc, a test's indices alone) has nothing the checks could hold against.
	if (geometry.vertices.empty() || geometry.layout.get_stride() == 0) {
		return;
	}
	check_data(file, geometry, false);
}

std::vector<std::pair<ds::bbox, std::uint32_t>> rnd::built_geometry_bounds(const geometry_desc& geometry)
{
	std::vector<std::pair<ds::bbox, std::uint32_t>> bounds;
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
	bounds.reserve(geometry.indices.size() / 3);
	for (std::size_t i = 0; i + 2 < geometry.indices.size(); i += 3) {
		ds::bbox box;
		for (std::size_t k = 0; k < 3; ++k) {
			ds::expand(box, uv_of(geometry.indices[i + k]));
		}
		bounds.push_back({ box, static_cast<std::uint32_t>(bounds.size()) });
	}
	return bounds;
}

void rnd::detail::rebuild_bounds(geometry_desc& geometry)
{
	PROFILE_SCOPE("geometry_desc bounds rebuild");
	geometry.bounds = built_geometry_bounds(geometry);
	geometry.rtree = {};
	for (const auto& [box, triangle] : geometry.bounds) {
		geometry.rtree.insert(box, triangle);
	}
}

void rnd::geometry_desc::deserialize(desc::desc_system&, const json::object& resource)
{
	detail::read_geometry_json(*this, resource);
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
	// Empty arrays keep the data __parent gave (copy_to); its bounds hold only while neither array is this desc's own.
	bool own_data = false;
	{
		auto& json_indices = resource.at("indices");
		ranges.indices = range_at(json_indices, "/indices");
		if (ranges.indices) {
			indices.clear();
		} else if (!json_indices.as_array().empty()) {
			indices = json::value_to<std::remove_cvref_t<decltype(indices)>>(json_indices);
			own_data = true;
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
			own_data = true;
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

	if (resource.contains("bounds")) {
		bounds = json::value_to<std::remove_cvref_t<decltype(bounds)>>(resource.at("bounds"));
	} else if (own_data) {
		bounds.clear();
	}
	PROFILE_SCOPE("geometry_desc rtree build");
	rtree = {};
	for (const auto& [box, triangle] : bounds) {
		rtree.insert(box, triangle);
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
	// Whatever the state says: a layout without vertices is never written empty (write_geometry_arrays throws).
	write_geometry_arrays(get_tag(), geometry_view_of(*this), resource);
}
