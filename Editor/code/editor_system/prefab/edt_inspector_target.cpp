#include "edt_inspector_target.h"
#include "edt_prefab_document.h"
#include "edt_scene_document.h"

bool edt::route_node_changed(const inspector_target& target, prefab_document* open_prefab, scene_document& level)
{
	if (target.is_prefab()) {
		return open_prefab && open_prefab->id() == target.prefab() && open_prefab->notify_edited();
	}
	level.commit("Edit node");
	return true;
}
