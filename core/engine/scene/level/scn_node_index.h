#pragma once
#include <entt/entt.hpp>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace scn
{
	// Address of a node inside a world: the node ids from the root of the spawn down to the node.
	// A node of the world itself has [id]; a child of a prefab instance has [instance_id, child_id].
	struct node_id_component {
		std::vector<uint64_t> path;
	};

	// path -> entity lookup, kept in reg.ctx(). Follows node_id_component through construct/update/destroy
	// hooks, so destroying an entity (or its whole hierarchy) unregisters it with no extra bookkeeping.
	class node_index {
	public:
		// Returns the registry's index, creating it (and hooking the registry) on first use.
		// Must run before the first node_id_component is added.
		static node_index& get(entt::registry& reg);

		// entt::null when no node has this path.
		entt::entity find(std::span<const uint64_t> path) const;

		std::size_t size() const { return m_entries.size(); }

	private:
		struct entry {
			entt::entity entity;
			std::vector<uint64_t> path;
		};

		static uint64_t hash_path(std::span<const uint64_t> path);

		void on_set(entt::registry& reg, entt::entity e);
		void on_destroy(entt::registry& reg, entt::entity e);
		void remove(entt::entity e);

		std::unordered_multimap<uint64_t, entry> m_entries;
		std::unordered_map<entt::entity, uint64_t> m_hash_of;
	};
}
