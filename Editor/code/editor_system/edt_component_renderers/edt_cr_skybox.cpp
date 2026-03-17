#include "edt_cr_internal.h"
#include "level/scn_prefab_desc.h"
#include "scn_skybox_desc.h"
#include <imgui.h>
#include <cstring>

void edt::edt_cr_skybox_register(edt::component_ui_registry& registry)
{
	registry.register_renderer<scn::skybox_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		bool changed = false;
		std::string mat;
		auto it = comp.overrides.find("material");
		if (it != comp.overrides.end() && it->value().is_string())
			mat = std::string(it->value().as_string());
		char buf[512];
		strncpy(buf, mat.c_str(), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		if (ImGui::InputText("Material Tag", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
			comp.overrides["material"] = std::string(buf);
			changed = true;
		}
		return changed;
	});
}
