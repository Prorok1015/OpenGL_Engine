#include "edt_cr_internal.h"
#include "edt_inspector_panel.h"
#include "level/scn_prefab_desc.h"
#include <imgui.h>
#include <string>

void edt::edt_cr_skin_register(edt::inspector_panel& panel)
{
	auto skin_renderer = [](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		auto it = comp.overrides.find("__parent");
		if (it != comp.overrides.end() && it->value().is_string())
			ImGui::TextDisabled("Source: %s", std::string(it->value().as_string()).c_str());
		else
			ImGui::TextDisabled("(no source)");
		return false;
	};
	panel.add_desc_renderer("skin_prototype_desc",     skin_renderer);
	panel.add_desc_renderer("skinning_prototype_desc", skin_renderer);
}
