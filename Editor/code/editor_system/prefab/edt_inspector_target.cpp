#include "edt_inspector_target.h"
#include "edt_prefab_document.h"
#include "edt_scene_document.h"
#include <format>
#include <string>

bool edt::route_node_changed(const inspector_target& target, prefab_document* open_prefab, scene_document& level)
{
	if (target.is_prefab()) {
		if (!open_prefab || open_prefab->id() != target.prefab()) return false;
		const scn::prefab_desc::prefab_node* node = open_prefab->working_node(target.path());
		return open_prefab->notify_edited(node && !node->name.empty() ? std::format("Edit {}", node->name) : std::string("Edit node"));
	}
	level.commit("Edit node");
	return true;
}
