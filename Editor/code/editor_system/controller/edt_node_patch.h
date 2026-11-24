#pragma once
#include "edt_scene_history.h"
#include <snk/scene/level/scn_world_desc.h>
#include <entt/entt.hpp>
#include <span>

namespace scn { class ecs_assembler; }

namespace edt
{
	// Applies field edits of `desc`'s nodes to their live entities in `reg` (the same registry a rebuild would replace): the
	// node's transform and name, and the components whose data changed, spawned again as a reload of the node does. All or
	// nothing: every entity and node is found before the first is touched; false when any is missing or the apply throws, and
	// then the caller rebuilds the world, which drops whatever was applied (#1190).
	[[nodiscard]] bool patch_live_nodes(const scn::world_desc& desc, entt::registry& reg, const scn::ecs_assembler& assembler,
		std::span<const scene_node_edit> edits);
}
