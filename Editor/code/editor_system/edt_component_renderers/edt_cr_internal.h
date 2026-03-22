#pragma once
#include <boost/json.hpp>
#include "edt_component_ui_registry.h"

namespace edt {

// Safe JSON-to-float: handles int64, uint64, and double storage kinds.
inline float json_number_to_float(const boost::json::value& v, float def)
{
	if (v.is_double())  return static_cast<float>(v.as_double());
	if (v.is_int64())   return static_cast<float>(v.as_int64());
	if (v.is_uint64())  return static_cast<float>(v.as_uint64());
	return def;
}

// Internal sub-registration functions — one per component type.
// Called exclusively from edt_component_renderers.cpp.
void edt_cr_camera_register(component_ui_registry& registry);
void edt_cr_light_register(component_ui_registry& registry);
void edt_cr_skybox_register(component_ui_registry& registry);
void edt_cr_skin_register(component_ui_registry& registry);
void edt_cr_animation_register(component_ui_registry& registry);

} // namespace edt
