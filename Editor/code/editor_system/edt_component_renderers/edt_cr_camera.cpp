#include "edt_cr_internal.h"
#include "level/scn_prefab_desc.h"
#include "scn_camera_desc.h"
#include <imgui.h>

void edt::edt_cr_camera_register(edt::component_ui_registry& registry)
{
	registry.register_renderer<scn::camera_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		bool changed = false;
		auto get_float = [&](const std::string& key, float def) -> float {
			auto it = comp.overrides.find(key);
			if (it != comp.overrides.end() && it->value().is_number())
				return edt::json_number_to_float(it->value(), def);
			return def;
		};
		float fov  = get_float("fov",           60.f);
		float near = get_float("near_distance",  0.01f);
		float far  = get_float("far_distance",   1000.f);
		if (ImGui::DragFloat("FOV",  &fov,  0.5f,   1.f,     179.f))           { comp.overrides["fov"]           = static_cast<double>(fov);  changed = true; }
		if (ImGui::DragFloat("Near", &near, 0.001f, 0.0001f, 10.f, "%.4f"))    { comp.overrides["near_distance"] = static_cast<double>(near); changed = true; }
		if (ImGui::DragFloat("Far",  &far,  1.f,    1.f,     100000.f))        { comp.overrides["far_distance"]  = static_cast<double>(far);  changed = true; }
		return changed;
	});
}
