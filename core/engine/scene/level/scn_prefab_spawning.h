#pragma once
#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <set>
#include <string>
#include <vector>

// What spawn_prefab and despawn keep in a world's ctx: not part of the engine's API, a game reads none of it.
namespace scn
{
	class ecs_assembler;

	// The assembler of the layer the world was built with and the world's root. Set each time a world is built from its
	// desc, and by bind_prefab_spawning after a world was restored from a snapshot; so after a module reload it names the
	// new layer's assembler, never the old one.
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

	namespace detail
	{
		// What the world_desc spawner calls: marks `root` as the world's root and binds it to `assembler`.
		void bind_world_root(entt::registry& world, const ecs_assembler& assembler, entt::entity root);

		// What world_context::apply_commands calls: removes every queued tree now.
		void flush_despawns(entt::registry& world);
	}
}
