#include "edt_cr_internal.h"
#include "level/scn_prefab_desc.h"
#include "scn_skinning_desc.h"
#include "scn_bone_desc.h"
#include <imgui.h>

void edt::edt_cr_skin_register(edt::component_ui_registry& registry)
{
	registry.register_renderer<scn::skinning_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		auto it = comp.overrides.find("bone_count");
		int count = (it != comp.overrides.end() && it->value().is_int64())
			? static_cast<int>(it->value().as_int64()) : 0;
		ImGui::TextDisabled("Bone count: %d", count);
		return false;
	});

	registry.register_renderer<scn::bone_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		auto it = comp.overrides.find("index");
		int idx = (it != comp.overrides.end() && it->value().is_int64())
			? static_cast<int>(it->value().as_int64()) : -1;
		ImGui::TextDisabled("Bone index: %d", idx);
		return false;
	});
}
