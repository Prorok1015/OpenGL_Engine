#include "edt_cr_internal.h"
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_skinning_desc.h>
#include <snk/scene/scn_bone_desc.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_reference.h>
#include <imgui.h>
#include <optional>
#include <string>

void edt::edt_cr_skin_register(edt::renderer_registrar& registry)
{
	registry.add<scn::skinning_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		auto it = comp.overrides.find("bone_count");
		int count = (it != comp.overrides.end() && it->value().is_int64())
			? static_cast<int>(it->value().as_int64()) : 0;
		ImGui::TextDisabled("Bone count: %d", count);

		if (auto tag_it = comp.overrides.find("skinning_tag"); tag_it != comp.overrides.end()) {
			if (const std::optional<res::reference> tag = res::reference_from_json(tag_it->value())) {
				const std::string shown = tag->path.view().empty() && !tag->guid.is_nil() ? res::to_text(tag->guid) : std::string(tag->path.view());
				ImGui::TextDisabled("Skinning tag: %s", shown.c_str());
			}
		}

		return false;
	});

	registry.add<scn::bone_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		auto it = comp.overrides.find("index");
		int idx = (it != comp.overrides.end() && it->value().is_int64())
			? static_cast<int>(it->value().as_int64()) : -1;
		ImGui::TextDisabled("Bone index: %d", idx);
		return false;
	});
}
