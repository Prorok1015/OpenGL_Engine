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

// Adds renderers to the inspector registry and keeps their registrations.
struct renderer_registrar {
	inspector_registry& inspectors;
	std::vector<registration>& registrations;

	template<typename T>
	void add(inspector_renderer fn)
	{
		registrations.push_back(inspectors.add(std::string(T::__type), std::move(fn)));
	}
};

// Internal sub-registration functions — one per component type.
// Called exclusively from edt_component_renderers.cpp.
void edt_cr_skin_register(renderer_registrar& registry);
void edt_cr_animation_register(renderer_registrar& registry);
void edt_cr_mesh_register(renderer_registrar& registry);

// The Visible box of mesh_node_desc (#560), apart from ImGui: the value the node draws with, and its edit.
bool mesh_node_visible(const scn::prefab_desc::prefab_comp_node& comp);
void set_mesh_node_visible(scn::prefab_desc::prefab_comp_node& comp, bool visible);

} // namespace edt
