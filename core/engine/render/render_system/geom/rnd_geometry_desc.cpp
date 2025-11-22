#include "rnd_geometry_desc.h"
#include "desc_system.h"

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

namespace std
{
	void tag_invoke(json::value_from_tag, json::value& jv, const std::byte& c)
	{
		//auto val = reinterpret_cast<const unsigned int&>(c);
		auto val = unsigned(c);
		jv = val;
	}

	std::byte tag_invoke(json::value_to_tag<std::byte>, const json::value& jv)
	{
		if (jv.is_int64()) {
			auto val = jv.as_int64();
			return std::byte(val);
		}

		if (jv.is_uint64()) {
			auto val = jv.as_uint64();
			return std::byte(val);
		}

		ASSERT_FAIL("rnd::geometry_desc::serialize", "Invalid type for byte");
		return std::byte(0);
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
		auto arr = jv.as_array();
		ASSERT_MSG(arr.size() == 2, "Invalid bbox array size");
		ds::bbox box;
		box.min.x = json::value_to<float>(arr[0].as_object().at("x"));
		box.min.y = json::value_to<float>(arr[0].as_object().at("y"));
		box.max.x = json::value_to<float>(arr[1].as_object().at("x"));
		box.max.y = json::value_to<float>(arr[1].as_object().at("y"));
		return box;
	}
}

void rnd::geometry_desc::deserialize(desc::desc_system& system, const json::object& resource)
{
	auto& json_layout = resource.at("layout");
	{
		std::vector<rnd::driver::BufferElement> elements;
		for (const auto& [name, type] : json_layout.as_object())
		{
			elements.push_back({ shader_data_type_from_string(type.as_string()), name });
		}

		layout = rnd::driver::BufferLayout{ elements };
	}

	{
		auto& json_indices = resource.at("indices");
		indices = json::value_to<decltype(indices)>(json_indices);
	}

	{
		auto& json_vertices = resource.at("vertices");
		vertices = json::value_to<decltype(vertices)>(json_vertices);
	}

	if (resource.contains("bounds"))
	{
		auto& json_bounds = resource.at("bounds");
		bounds = json::value_to<decltype(bounds)>(json_bounds);
		rtree.build(bounds);
	}
}

void rnd::geometry_desc::serialize(json::object& resource) const
{
	resource["layout"] = json::value_from(layout);
	resource["indices"] = json::value_from(indices);
	resource["vertices"] = json::value_from(vertices);
	resource["bounds"] = json::value_from(bounds);
}
