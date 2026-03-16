#include "edt_cr_internal.h"
#include "edt_inspector_panel.h"
#include "level/scn_prefab_desc.h"
#include <imgui.h>
#include <boost/json.hpp>
#include <glm/glm.hpp>
#include <string>

void edt::edt_cr_light_register(edt::inspector_panel& panel)
{
	panel.add_desc_renderer("directional_light_desc", [](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		bool changed = false;
		auto get_vec3 = [&](const std::string& key, glm::vec3 def) -> glm::vec3 {
			auto it = comp.overrides.find(key);
			if (it != comp.overrides.end() && it->value().is_array() && it->value().as_array().size() == 3) {
				const auto& arr = it->value().as_array();
				if (!arr[0].is_number() || !arr[1].is_number() || !arr[2].is_number())
					return def;
				return { edt::json_number_to_float(arr[0], def.x),
				         edt::json_number_to_float(arr[1], def.y),
				         edt::json_number_to_float(arr[2], def.z) };
			}
			return def;
		};
		auto set_vec3 = [&](const std::string& key, glm::vec3 v) {
			comp.overrides[key] = boost::json::array{ static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z) };
		};
		glm::vec3 dir      = get_vec3("direction", { 0.f, -1.f, 0.f });
		glm::vec3 diffuse  = get_vec3("diffuse",   { 1.f,  1.f, 1.f });
		glm::vec3 ambient  = get_vec3("ambient",   { 0.1f, 0.1f, 0.1f });
		glm::vec3 specular = get_vec3("specular",  { 1.f,  1.f, 1.f });
		if (ImGui::DragFloat3("Direction", &dir.x, 0.01f, -1.f, 1.f))  { set_vec3("direction", dir);      changed = true; }
		if (ImGui::ColorEdit3("Diffuse",   &diffuse.x))                 { set_vec3("diffuse",   diffuse);  changed = true; }
		if (ImGui::ColorEdit3("Ambient",   &ambient.x))                 { set_vec3("ambient",   ambient);  changed = true; }
		if (ImGui::ColorEdit3("Specular",  &specular.x))                { set_vec3("specular",  specular); changed = true; }
		return changed;
	});
}
