#include "edt_cr_internal.h"
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_directional_light_desc.h>
#include <imgui.h>
#include <boost/json.hpp>
#include <glm/glm.hpp>

void edt::edt_cr_light_register(edt::renderer_registrar& registry)
{
	registry.add<scn::directional_light_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		bool changed = false;
		// Defaults come from the desc itself so a light without overrides shows what is actually rendered.
		const scn::directional_light_desc def;
		auto& ov = comp.overrides;
		glm::vec4 dir      = edt::read_vec4_override(ov, "direction", def.direction);
		glm::vec4 diffuse  = edt::read_vec4_override(ov, "diffuse",   def.diffuse);
		glm::vec4 ambient  = edt::read_vec4_override(ov, "ambient",   def.ambient);
		glm::vec4 specular = edt::read_vec4_override(ov, "specular",  def.specular);
		// The 3-component widgets leave w untouched.
		if (ImGui::DragFloat3("Direction", &dir.x, 0.01f, -1.f, 1.f))  { edt::write_vec4_override(ov, "direction", dir);      changed = true; }
		if (ImGui::ColorEdit3("Diffuse",   &diffuse.x))                 { edt::write_vec4_override(ov, "diffuse",   diffuse);  changed = true; }
		if (ImGui::ColorEdit3("Ambient",   &ambient.x))                 { edt::write_vec4_override(ov, "ambient",   ambient);  changed = true; }
		if (ImGui::ColorEdit3("Specular",  &specular.x))                { edt::write_vec4_override(ov, "specular",  specular); changed = true; }
		return changed;
	});
}
