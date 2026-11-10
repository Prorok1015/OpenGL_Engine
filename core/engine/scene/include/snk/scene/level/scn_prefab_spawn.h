#pragma once
#include <snk/common/snk_api.h>
#include <snk/common/eng_transform_3d.hpp>
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/scene/level/scn_prefab_desc.h>
#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <optional>

namespace scn
{
	class ecs_assembler;

	// A world restored from a snapshot is not built from its desc: binds its root (the entity with world_root_component)
	// to `assembler`. False when the world has no such root.
	SNK_API bool bind_prefab_spawning(entt::registry& world, const ecs_assembler& assembler);

	// Builds a prefab under `parent` (entt::null: the world's root) now and returns its root: the root has a node path
	// of its own (the parent's path and a new id), so its nodes never share a path with the world's. `local` is the root's
	// local transform; none keeps the prefab root's own. A tag not loaded yet is loaded now, which stalls the frame: load
	// it ahead and pass the handle. entt::null, with a line in the log, when the world was not built from a desc, the
	// parent is gone or the prefab does not load.
	// A hot reload of the prefab the parent belongs to keeps the spawned tree. Entity handles do not survive a rebuild
	// of the world (reload_context, Play's restore); outside Play the spawned entities go with such a rebuild.
	SNK_API entt::entity spawn_prefab(entt::registry& world, const res::tag& prefab,
		std::optional<eng::transform3d> local = std::nullopt, entt::entity parent = entt::null);
	SNK_API entt::entity spawn_prefab(entt::registry& world, const res::res_handle<prefab_desc>& prefab,
		std::optional<eng::transform3d> local = std::nullopt, entt::entity parent = entt::null);

	// Removes `root` and everything below it at the end of the current phase (the world's command flush), unlinked from
	// its parent first, so no system of a later phase meets a half-removed tree. Asking twice, or for an entity that is
	// gone, does nothing; the world's root and any entity without a parent are refused with a line in the log.
	SNK_API void despawn(entt::registry& world, entt::entity root);
}
