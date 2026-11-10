#include <snk/scene/level/scn_prefab_spawn.h>
#include "scn_hierarchy_destroy.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_node_id.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <entt/entt.hpp>
#include <algorithm>
#include <filesystem>
#include <utility>

namespace {
	bool is_world_root(const entt::registry& world, entt::entity e)
	{
		const auto* spawning = world.ctx().find<scn::prefab_spawning>();
		return spawning && spawning->world_root == e;
	}

	// The world's root is the one entity with the empty node path and no parent.
	entt::entity find_world_root(entt::registry& world)
	{
		for (auto&& [e, id] : world.view<scn::node_id_component>().each()) {
			if (id.path.empty() && !world.all_of<scn::parent_component>(e)) return e;
		}
		return entt::null;
	}

	std::string name_of(const res::tag& tag)
	{
		std::string name = std::filesystem::path(std::string(tag.relative())).filename().string();
		// "enemy.prefab.desc" is the node "enemy".
		if (const auto dot = name.find('.'); dot != std::string::npos && dot > 0) name.resize(dot);
		return name;
	}
}

bool scn::bind_prefab_spawning(entt::registry& world, const ecs_assembler& assembler)
{
	const entt::entity root = find_world_root(world);
	if (root == entt::null) return false;
	world.ctx().insert_or_assign(prefab_spawning{ .assembler = &assembler, .world_root = root, .waited_for = {} });
	return true;
}

entt::entity scn::spawn_prefab(entt::registry& world, const res::tag& prefab, const glm::mat4& local, entt::entity parent)
{
	auto* spawning = world.ctx().find<prefab_spawning>();
	if (!spawning || !spawning->assembler) {
		egLOG_WARN("scene", "spawn_prefab '{}': the world was not built from a desc, nothing to spawn with", prefab.view());
		return entt::null;
	}
	auto& resources = spawning->assembler->get_resource_system();
	auto handle = resources.require<prefab_desc>(prefab);
	if (!handle.is_ready() && !handle.has_error()) {
		if (spawning->waited_for.insert(std::string(prefab.view())).second) {
			egLOG_WARN("scene", "spawn_prefab '{}': not loaded yet, the frame waits for it; load it ahead and pass the handle",
				prefab.view());
		}
		handle = resources.require_sync<prefab_desc>(prefab);
	}
	return spawn_prefab(world, handle, local, parent);
}

entt::entity scn::spawn_prefab(entt::registry& world, const res::res_handle<prefab_desc>& prefab, const glm::mat4& local,
	entt::entity parent)
{
	const auto* spawning = world.ctx().find<prefab_spawning>();
	if (!spawning || !spawning->assembler) {
		egLOG_WARN("scene", "spawn_prefab '{}': the world was not built from a desc, nothing to spawn with", prefab.get_tag().view());
		return entt::null;
	}
	if (parent == entt::null) parent = spawning->world_root;
	if (!world.valid(parent)) {
		egLOG_WARN("scene", "spawn_prefab '{}': the parent is gone", prefab.get_tag().view());
		return entt::null;
	}
	if (!prefab.is_ready()) {
		egLOG_WARN("scene", "spawn_prefab '{}': the prefab did not load", prefab.get_tag().view());
		return entt::null;
	}
	const auto desc = prefab.get_sync();
	if (!desc) return entt::null;

	node_index::get(world);
	std::vector<std::uint64_t> path;
	if (const auto* parent_id = world.try_get<node_id_component>(parent)) path = parent_id->path;
	path.push_back(desc::new_node_id());

	const entt::entity root = world.create();
	world.emplace<node_id_component>(root, std::move(path));
	world.emplace<parent_component>(root, parent);
	world.get_or_emplace<children_component>(parent).children.push_back(root);
	world.emplace<local_transform>(root, local);
	assemble_prefab(*spawning->assembler, world, root, *desc, name_of(prefab.get_tag()), prefab.get_tag());
	return root;
}

void scn::despawn(entt::registry& world, entt::entity root)
{
	if (!world.valid(root)) return;
	if (is_world_root(world, root) || !world.all_of<parent_component>(root)) {
		egLOG_WARN("scene", "despawn: the world's root and entities without a parent are not removed this way");
		return;
	}
	auto& queue = world.ctx().emplace<despawn_queue>();
	if (std::ranges::find(queue.roots, root) == queue.roots.end()) queue.roots.push_back(root);
}

void scn::flush_despawns(entt::registry& world)
{
	auto* queue = world.ctx().find<despawn_queue>();
	if (!queue || queue->roots.empty()) return;
	// Removing a tree may hold another queued root (a child of it): whatever is gone by its turn is skipped.
	const std::vector<entt::entity> roots = std::exchange(queue->roots, {});
	for (const entt::entity root : roots) {
		if (!world.valid(root)) continue;
		if (const auto* parent = world.try_get<parent_component>(root); parent && world.valid(parent->parent)) {
			if (auto* siblings = world.try_get<children_component>(parent->parent)) {
				std::erase(siblings->children, root);
			}
		}
		detail::destroy_hierarchy(world, root);
	}
}
