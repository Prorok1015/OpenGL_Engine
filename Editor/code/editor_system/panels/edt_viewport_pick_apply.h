#pragma once
#include "edt_viewport_click.h"

namespace edt
{
	class scene_hierarchy_panel;
	class selection_service;

	// What the selection does with a click or a box of the viewport (#1327). A click without a modifier that hit one node selects it as
	// the hierarchy's own click does (a node inside a prefab instance is selected read-only through `select_path`); a click on empty space
	// clears the selection; a box, a click with Shift or Ctrl, or several nodes go to `select_paths` (a node inside an instance stands
	// for the instance). Add and remove with no node change nothing. A pick the hierarchy does not know leaves the selection as it is.
	void apply_viewport_pick(scene_hierarchy_panel& hierarchy, selection_service& selection, const viewport_pick& pick);
}
