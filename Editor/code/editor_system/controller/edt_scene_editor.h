#pragma once
#include <snk/scene/level/scn_prefab_desc.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <functional>
#include <glm/vec3.hpp>
#include <entt/fwd.hpp>

namespace edt
{
	struct shared_state;

	class scene_editor
	{
	public:
		using prefab_node = scn::prefab_desc::prefab_node;

		// Where a node-id path (as in scn::node_id_component) leads in the edited desc. `owner` is the deepest node
		// of that desc on the path; `target` are the ids below it, inside the prefab it instantiates, so a
		// non-empty target means the node can only be changed through override records on `owner`.
		struct node_address {
			prefab_node* owner = nullptr;
			std::vector<uint64_t> target;
		};

		explicit scene_editor(shared_state& state);

		void create_entity(const std::string& type_name);
		void create_entity_from_node(prefab_node node);
		void delete_entity(uint64_t node_id);
		void delete_entities(const std::vector<uint64_t>& node_ids);
		void duplicate_entity(uint64_t node_id);
		void duplicate_entities(const std::vector<uint64_t>& node_ids);
		// new_parent_id 0 = the root. insert_index: -1 = as last child of the parent, 0 / 1 = before / after the node new_parent_id.
		void reparent_entity(uint64_t node_id, uint64_t new_parent_id, int insert_index);
		void on_transform_committed(std::span<const uint64_t> path, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale);

		void serialize_and_push();
		void sync_ecs_transforms_to_desc();

		// Copies a live transform into the node. A node that references a desc and has no transform of its own is
		// placed by the referenced prefab; that placement is not the user's, so it is left alone.
		static void write_live_transform(prefab_node& node, const glm::mat4& local);

		// Name for a new root node; only for display, nodes are identified by id.
		std::string make_unique_key(const std::string& base_name) const;

		static prefab_node* find_node_by_id(std::vector<prefab_node>& nodes, uint64_t node_id);
		static bool remove_node_by_id(std::vector<prefab_node>& nodes, uint64_t node_id);
		static std::vector<prefab_node>* find_parent_children(std::vector<prefab_node>& nodes, uint64_t node_id);
		static node_address resolve_node_path(std::vector<prefab_node>& nodes, std::span<const uint64_t> path);

		// Callbacks set by the coordinator
		std::function<void()> on_before_modify;
		std::function<void()> on_after_push;
		std::function<void(entt::registry&)> on_save_camera;
		std::function<void(entt::registry&)> on_inject_camera;

	private:
		shared_state& m_state;
	};
}
