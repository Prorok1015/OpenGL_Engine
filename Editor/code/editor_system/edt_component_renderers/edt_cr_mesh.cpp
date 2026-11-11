#include "edt_cr_internal.h"
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_mesh_node_desc.h>
#include <imgui.h>

namespace {
	bool parent_hides(const scn::prefab_desc::prefab_comp_node& comp)
	{
		if (!comp.parent_desc.is_valid() || !comp.parent_desc.is_ready()) return false;
		const auto parent = comp.parent_desc.get();
		const auto* mesh = dynamic_cast<const scn::mesh_node_desc*>(parent.get());
		return mesh && !mesh->visible;
	}

	// What the node draws with: its parent's fields under its own.
	boost::json::object shown_fields(const scn::prefab_desc::prefab_comp_node& comp)
	{
		boost::json::object shown;
		if (comp.parent_desc.is_valid() && comp.parent_desc.is_ready()) comp.parent_desc.get()->serialize(shown);
		for (const auto& member : comp.overrides) shown.insert_or_assign(member.key(), member.value());
		return shown;
	}
}

bool edt::mesh_node_visible(const scn::prefab_desc::prefab_comp_node& comp)
{
	if (const auto* value = comp.overrides.if_contains("visible"); value && value->is_bool()) return value->as_bool();
	return !parent_hides(comp);
}

void edt::set_mesh_node_visible(scn::prefab_desc::prefab_comp_node& comp, bool visible)
{
	// Written out even when true: the base may hide the mesh by an override of its own node, which the renderer
	// cannot see, and override_edit::diff keeps no removed key.
	comp.overrides["visible"] = visible;
}

void edt::edt_cr_mesh_register(edt::renderer_registrar& registry)
{
	registry.add<scn::mesh_node_desc>([](scn::prefab_desc::prefab_comp_node& comp) -> bool {
		bool visible = mesh_node_visible(comp);
		bool changed = false;
		if (ImGui::Checkbox("Visible", &visible)) {
			set_mesh_node_visible(comp, visible);
			changed = true;
		}
		const boost::json::object shown = shown_fields(comp);
		for (const char* key : { "geometry", "material", "vx_begin", "vx_end", "ind_begin", "ind_end" }) {
			if (const auto* value = shown.if_contains(key)) {
				ImGui::TextDisabled("%s: %s", key, boost::json::serialize(*value).c_str());
			}
		}
		return changed;
	});
}
