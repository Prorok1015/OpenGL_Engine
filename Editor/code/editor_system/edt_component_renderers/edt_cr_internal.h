#pragma once
#include <boost/json.hpp>
#include <glm/glm.hpp>
#include <snk/editor/edt_inspector_registry.h>
#include <string_view>
#include <utility>
#include <vector>

namespace edt {

// Safe JSON-to-float: handles int64, uint64, and double storage kinds.
inline float json_number_to_float(const boost::json::value& v, float def)
{
	if (v.is_double())  return static_cast<float>(v.as_double());
	if (v.is_int64())   return static_cast<float>(v.as_int64());
	if (v.is_uint64())  return static_cast<float>(v.as_uint64());
	return def;
}

// vec4 fields are stored as 4-element arrays everywhere (desc serialize, scn_glm_json_convert), so the editor
// reads/writes all 4 components; a 3-element array would be rejected by the loader.
// Anything that is not an array of 4 numbers yields `def`.
inline glm::vec4 read_vec4_override(const boost::json::object& overrides, std::string_view key, glm::vec4 def)
{
	const auto* v = overrides.if_contains(key);
	if (!v || !v->is_array() || v->as_array().size() != 4)
		return def;
	const auto& a = v->as_array();
	for (const auto& n : a) {
		if (!n.is_number()) return def;
	}
	return { json_number_to_float(a[0], def.x), json_number_to_float(a[1], def.y),
	         json_number_to_float(a[2], def.z), json_number_to_float(a[3], def.w) };
}

inline void write_vec4_override(boost::json::object& overrides, std::string_view key, glm::vec4 v)
{
	overrides[key] = boost::json::array{ static_cast<double>(v.x), static_cast<double>(v.y),
	                                     static_cast<double>(v.z), static_cast<double>(v.w) };
}

// Adds renderers to the inspector registry and keeps their registrations.
struct renderer_registrar {
	inspector_registry& inspectors;
	std::vector<registration>& registrations;

	template<typename T>
	void add(inspector_renderer fn)
	{
		registrations.push_back(inspectors.add<T>(std::move(fn)));
	}
};

// Internal sub-registration functions — one per component type.
// Called exclusively from edt_component_renderers.cpp.
void edt_cr_light_register(renderer_registrar& registry);
void edt_cr_skybox_register(renderer_registrar& registry);
void edt_cr_skin_register(renderer_registrar& registry);
void edt_cr_animation_register(renderer_registrar& registry);

} // namespace edt
