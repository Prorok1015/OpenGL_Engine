#pragma once
#include <snk/common/snk_api.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/scene/level/scn_prefab_desc.h>
#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <set>
#include <string>
#include <vector>

namespace scn
{
	class ecs_assembler;

	// What spawn_prefab needs in a world: the assembler of the layer the world was built with and the world's root.
	// Set by the world_desc spawner each time a world is built from its desc, and by bind_prefab_spawning after a world
	// was restored from a snapshot; so after a module reload it names the new layer's assembler, never the old one.
	struct prefab_spawning
	{
		const ecs_assembler* assembler = nullptr;
		entt::entity world_root{ entt::null };
		// Tags spawn_prefab had to wait for once: one warning per tag.
		std::set<std::string> waited_for;
	};

	// The roots despawn was asked to remove; world_context::apply_commands empties it after the phase it was asked in.
	struct despawn_queue
	{
		std::vector<entt::entity> roots;
	};

	// A world restored from a snapshot is not built from its desc: binds its root (the entity with the empty node path
	// and no parent) to `assembler`. False when the world has no such root.
	SNK_API bool bind_prefab_spawning(entt::registry& world, const ecs_assembler& assembler);

	// Builds a prefab under `parent` (entt::null: the world's root) now and returns its root: the root has a node path
	// of its own (the parent's path and a new id), so its nodes never share a path with the world's, and `local` as its
	// local transform (identity: the prefab root's own). A tag not loaded yet is loaded now, which stalls the frame:
	// load it ahead and pass the handle. entt::null, with a line in the log, when the world was not built from a desc,
	// the parent is gone or the prefab does not load.
	// Outside Play the entities it makes are not part of the level's document and go when the world is rebuilt.
	SNK_API entt::entity spawn_prefab(entt::registry& world, const res::tag& prefab, const glm::mat4& local = glm::mat4{ 1.0f },
		entt::entity parent = entt::null);
	SNK_API entt::entity spawn_prefab(entt::registry& world, const res::res_handle<prefab_desc>& prefab,
		const glm::mat4& local = glm::mat4{ 1.0f }, entt::entity parent = entt::null);

	// Removes `root` and everything below it at the end of the current phase (the world's command flush), unlinked from
	// its parent first, so no system of a later phase meets a half-removed tree. Asking twice, or for an entity that is
	// gone, does nothing; the world's root and any entity without a parent are refused with a line in the log.
	SNK_API void despawn(entt::registry& world, entt::entity root);

	// What world_context::apply_commands calls: removes every queued tree now.
	SNK_API void flush_despawns(entt::registry& world);
}
