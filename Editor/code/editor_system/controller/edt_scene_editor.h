#pragma once
#include "level/scn_prefab_desc.h"
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

		explicit scene_editor(shared_state& state);

		void create_entity(const std::string& type_name);
		void create_entity_from_node(prefab_node node);
		void delete_entity(const std::string& name);
		void duplicate_entity(const std::string& name);
		void reparent_entity(const std::string& node_name, const std::string& new_parent_name, int insert_index);
		void on_transform_committed(const std::string& name, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale);

		void serialize_and_push();
		void sync_ecs_transforms_to_desc();

		std::string make_unique_key(const std::string& base_name) const;

		static prefab_node* find_node_by_name(std::vector<prefab_node>& nodes, const std::string& name);
		static bool remove_node_by_name(std::vector<prefab_node>& nodes, const std::string& name);
		static std::vector<prefab_node>* find_parent_children(std::vector<prefab_node>& nodes, const std::string& name);

		// Callbacks set by the coordinator
		std::function<void()> on_before_modify;
		std::function<void()> on_after_push;
		std::function<void(entt::registry&)> on_save_camera;
		std::function<void(entt::registry&)> on_inject_camera;

	private:
		shared_state& m_state;
	};
}
