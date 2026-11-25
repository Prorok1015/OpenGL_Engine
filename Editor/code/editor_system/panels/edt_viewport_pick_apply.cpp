#include "edt_viewport_pick_apply.h"
#include "edt_scene_hierarchy_panel.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_selection_service.h>

void edt::apply_viewport_pick(scene_hierarchy_panel& hierarchy, selection_service& selection, const viewport_pick& pick)
{
	if (pick.box || pick.op != pick_op::replace || pick.paths.size() > 1) {
		if (!pick.box && pick.op == pick_op::replace && pick.paths.empty()) {
			selection.clear();
			return;
		}
		hierarchy.select_paths(pick.paths, pick.op);
		return;
	}
	if (pick.paths.empty()) {
		selection.clear();
		return;
	}
	// Ids below a prefab instance are not nodes of the desc: the hierarchy takes them as the target tail.
	const bool selected = select_longest_own_prefix(pick.paths.front(), [&hierarchy](const std::vector<std::uint64_t>& full, const std::vector<std::uint64_t>& target) {
		return hierarchy.select_path(full, target);
	});
	if (!selected)
		egLOG_WARN("editor", "viewport pick: the hierarchy does not know the picked node, the selection is kept");
}
