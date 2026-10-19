#pragma once
#include <snk/ecs/ecs_entity.h>
#include <snk/editor/edt_editor_events.h>
#include <functional>
#include <vector>

namespace edt
{
	class scene_hierarchy_panel;
	class inspector_panel;
	class scene_editor;
	class world_controller;
	class selection_service;
	struct shared_state;

	// Routes the selection between the hierarchy, the inspector and the viewport through selection_service
	// and editor_events instead of direct callbacks. Sets scene_editor::on_before_modify / on_after_push,
	// world_controller::on_world_switched and the hierarchy's node-selected callback. The returned
	// connections keep the subscriptions alive; subscription order is the order the old callbacks ran in.
	// The hierarchy panel still draws and edits its own selection; a selection made elsewhere is not mirrored
	// into it yet (nothing makes one today), only a cleared one is.
	[[nodiscard]] std::vector<connection> wire_selection(
		editor_events& events,
		selection_service& service,
		shared_state& state,
		scene_editor& scene_ed,
		world_controller& world_ctrl,
		scene_hierarchy_panel& hierarchy,
		inspector_panel& inspector,
		std::function<void(ecs::entity)> set_viewport_entity);
}
