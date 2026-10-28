#pragma once
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <unordered_map>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/level/scn_override_record.h>
#include <glm/glm.hpp>
#include <snk/common/snk_api.h>

namespace scn {
	class SNK_API prefab_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "prefab_desc";
		using base_type = desc::desc_base;

		// A pointer write that has to land after serialize(parent) + overrides are merged. Only built while an
		// instance is expanded (override records become edits); never stored in a desc or saved.
		struct comp_edit {
			std::string path; // RFC 6901; "" merges the object `value` into the component
			boost::json::value value;
		};

		struct prefab_comp_node {
			std::string type_name{};
			res::res_handle<desc::desc_base> parent_desc;
			boost::json::object overrides{};
			std::vector<comp_edit> edits{};
		};

		struct prefab_node {
			std::string name;
			uint64_t node_id = 0; // persistent identity, unique within one desc file; 0 = invalid
			glm::vec3 position{ 0.0f };
			glm::vec3 rotation{ 0.0f };
			glm::vec3 scale{ 1.0f };
			// The node's transform is spelled out (a "transform" object exists), even if every value equals the
			// default. For a node that instantiates a prefab it decides whether the node's own transform or the
			// prefab root's (with target-[] records) places the instance.
			bool has_transform = false;

			std::unordered_map<std::string, prefab_comp_node> components;
			std::vector<prefab_node> children;
			// Changes this node makes to the content it instantiates (see override_record). A node that references
			// a prefab applies them to that prefab; any other node applies them to its own subtree.
			std::vector<override_record> overrides;

			glm::mat4 get_local_transform() const
			{
				glm::mat4 t = glm::translate(glm::mat4(1.0f), position);
				glm::mat4 r = glm::yawPitchRoll(glm::radians(rotation.y), glm::radians(rotation.x), glm::radians(rotation.z));
				glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
				return t * r * s;
			}
		};

		prefab_desc() = default;
		~prefab_desc() = default;
		prefab_desc(const prefab_desc&) = default;
		prefab_desc(prefab_desc&&) = default;
		prefab_desc& operator=(const prefab_desc&) = default;
		prefab_desc& operator=(prefab_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			copy_to_same(*this, other);
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		prefab_node& get_root() { return root; }
		const prefab_node& get_root() const { return root; }
	private:
		prefab_node root;
	};

	struct prefab_instance {
		res::tag source_prefab;
		scn::prefab_desc::prefab_node local_overrides;
	};

	// Gives every node of the subtree a fresh random id (editor duplicate).
	SNK_API void regenerate_node_ids(scn::prefab_desc::prefab_node& node);

	// prefab_desc / world_desc v1 -> v2: `children` becomes an array of { "__id", "name", ... } nodes and
	// components spell out their __type. Registered with the desc_system by register_scene_types.
	SNK_API void migrate_prefab_v1(boost::json::object& obj);

	// prefab_desc / world_desc v2 -> v3: the override layer of an explicit nested instance (a component
	// { "__type": "prefab_desc", "__parent": X }) becomes `overrides` records on the instance node.
	SNK_API void migrate_prefab_v2(boost::json::object& obj);

	enum class reconcile_mode {
		spawn,  // `live` is a fresh entity: create everything below it
		reload, // `live` already has children: update the ones that still exist, add new ones, destroy the rest
	};

	// The single place that turns prefab nodes into entities. If `node` references a prefab, that prefab's
	// content (with the node's override records applied and the node's own components and children added) is
	// what gets built, and the entity remembers the prefab as its prefab_instance. Children are matched with
	// the live children on a reload by node id. Every spawned node gets its node_id_component, which fills
	// the registry's node_index.
	SNK_API void reconcile(
		const ecs_assembler& assembler,
		entt::registry& reg,
		entt::entity live,
		const prefab_desc::prefab_node& node,
		reconcile_mode mode);

	// What reconcile builds for `node` before it spawns anything: the referenced prefab (itself expanded) plus the
	// node's own components and children, then the node's override records, those forwarded from above included.
	// False (`out` untouched) for a plain node without records; `source` is the prefab's tag, empty if none.
	// Nested instances below `out` stay unexpanded: call it again on the child you descend into.
	SNK_API bool expand_instance(
		const ecs_assembler& assembler,
		const prefab_desc::prefab_node& node,
		prefab_desc::prefab_node& out,
		res::tag& source);

	// A component as reconcile spawns it: serialize(parent), the component's own overrides, then its edits.
	SNK_API boost::json::object compose_component_json(const prefab_desc::prefab_comp_node& cnode, const desc::desc_base* parent);

	SNK_API void trigger_hot_reload(scn::ecs_assembler& assembler, entt::registry& reg, res::res_handle<desc::desc_base> changed_prefab);
	SNK_API void assemble_prefab(const scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string_view name, res::tag source_prefab);
}