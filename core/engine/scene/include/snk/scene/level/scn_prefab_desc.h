#pragma once
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
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
		// The file whose content the instance node is (empty when unknown, e.g. a plain prefab root); hot reload opens it (#993).
		res::tag content_of;
	};

	// Gives every node of the subtree a fresh random id (editor duplicate).
	SNK_API void regenerate_node_ids(scn::prefab_desc::prefab_node& node);

	// prefab_desc / world_desc v1 -> v2: `children` becomes an array of { "__id", "name", ... } nodes and
	// components spell out their __type. Registered with the desc_system by register_scene_types.
	SNK_API void migrate_prefab_v1(boost::json::object& obj);

	// prefab_desc / world_desc v2 -> v3: the override layer of an explicit nested instance (a component
	// { "__type": "prefab_desc", "__parent": X }) becomes `overrides` records on the instance node.
	SNK_API void migrate_prefab_v2(boost::json::object& obj);

	// prefab_desc / world_desc v3 -> v4: the pose of a camera moves from camera_desc.transform into its node's transform
	// (camera_desc v1 -> v2, see migrate_camera_transform_into_node).
	SNK_API void migrate_prefab_v3(boost::json::object& obj);
	// The same, telling `context` about an override of a camera pose it has to drop.
	SNK_API void migrate_prefab_v3(boost::json::object& obj, desc::migration_context& context);

	enum class reconcile_mode {
		spawn,  // `live` is a fresh entity: create everything below it
		reload, // `live` already has children: update the ones that still exist, add new ones, destroy the rest
	};

	// The single place that turns prefab nodes into entities. If `node` references a prefab, that prefab's
	// content (with the node's override records applied and the node's own components and children added) is
	// what gets built, and the entity remembers the prefab as its prefab_instance. Children are matched with
	// the live children on a reload by node id. Every spawned node gets its node_id_component, which fills
	// the registry's node_index. The file `node` is content of is unknown here: see the overload below.
	SNK_API void reconcile(
		const ecs_assembler& assembler,
		entt::registry& reg,
		entt::entity live,
		const prefab_desc::prefab_node& node,
		reconcile_mode mode);
	// `file`: the file `node` is content of, empty when unknown. It is open while `node` is built, as in assemble_prefab:
	// a record of it may instantiate the prefab `node` instantiates, one that instantiates `file` itself is refused.
	SNK_API void reconcile(
		const ecs_assembler& assembler,
		entt::registry& reg,
		entt::entity live,
		const prefab_desc::prefab_node& node,
		reconcile_mode mode,
		const res::tag& file);

	class world_desc;
	// Builds `desc` (a world) on `e` with `file`, the world's file, open, as a level load builds it; empty: plain
	// spawn_from_desc. The root gets no prefab_instance: a hot reload of the file must not rebuild an edited world.
	SNK_API void spawn_world(
		const ecs_assembler& assembler,
		entt::registry& reg,
		entt::entity e,
		const world_desc& desc,
		std::string_view name,
		const res::tag& file);

	// What reconcile builds for `node` before it spawns anything: the referenced prefab (itself expanded) plus the
	// node's own components and children, then the node's override records, those forwarded from above included.
	// False (`out` untouched) for a plain node without records; `source` is the prefab's tag, empty if none.
	// Nested instances below `out` stay unexpanded: call it again on the child you descend into.
	SNK_API bool expand_instance(
		const ecs_assembler& assembler,
		const prefab_desc::prefab_node& node,
		prefab_desc::prefab_node& out,
		res::tag& source);
	// `file`: the file whose content `node` is, empty when unknown (as the overload above); it is open while `node` is
	// expanded, as in the reconcile overload that names it.
	SNK_API bool expand_instance(
		const ecs_assembler& assembler,
		const prefab_desc::prefab_node& node,
		prefab_desc::prefab_node& out,
		res::tag& source,
		const res::tag& file);

	// A record of an instance that changes something the prefab no longer has: its node, or the component it sets or removes.
	// Nothing deletes it by itself (it stays in the file, apply skips it with a warning); an editor shows it as stale.
	struct stale_override
	{
		// dropped_by_migration: a step of the component's type removed the field the record sets; duplicated_by_migration: the
		// steps put its value in more than one place, so it is not applied, and prune_stale_records keeps it for a person to
		// decide; not_migrated: the steps cannot be followed (scn::record_migration::failed), it is applied as written and may
		// well work, so prune_stale_records keeps it too; field_gone_without_version: a record older than versions sets a field
		// the type has not got any more (#1255). Each names the record and the type in `what`.
		enum class reason : std::uint8_t {
			node_gone, component_gone, dropped_by_migration, duplicated_by_migration, not_migrated, field_gone_without_version
		};
		// Number of the record among `node.overrides` of the node it was found for.
		std::size_t index = 0;
		reason why = reason::node_gone;
		// For a person: what is gone, by node id and component key.
		std::string what;
	};
	// The stale records of `node`, in the order of `node.overrides`: judged by expanding the node, so a record that an earlier one
	// makes meaningful (an added node) is not stale, and one handed down to a nested instance is judged there (each nested instance
	// is expanded once; one that instantiates a prefab it is inside is refused, as in a spawn). Applies nothing. A node that cannot
	// be expanded reports nothing. stale_override is in gs::module_layout_key (#1449).
	SNK_API std::vector<stale_override> find_stale_records(const ecs_assembler& assembler, const prefab_desc::prefab_node& node);
	// Removes them from `node.overrides` (what the editor's "clear stale overrides" and a cook do), but for duplicated_by_migration
	// and not_migrated, which a person decides; returns how many went.
	SNK_API std::size_t prune_stale_records(const ecs_assembler& assembler, prefab_desc::prefab_node& node);
	// Rewrites the set records of `node` written against an older version of their component's type to the current one (#1255),
	// and gives a record without a version the current one: what the editor does when it loads a document, since a step of the
	// file cannot see the type an instance's record edits. Expands the node as find_stale_records does; a record the steps drop or
	// duplicate is left as it is (find_stale_records names it). Returns how many records changed.
	SNK_API std::size_t migrate_records(const ecs_assembler& assembler, prefab_desc::prefab_node& node);

	// A component as reconcile spawns it: serialize(parent), the component's own overrides, then its edits.
	SNK_API boost::json::object compose_component_json(const prefab_desc::prefab_comp_node& cnode, const desc::desc_base* parent);
	// The same with the typed objects of serialize(parent) that have no "__version" given the current one in `descs`
	// (#1236): they come from the memory of this process, and a module built before #1236 writes its inline descs without
	// it, which a read takes for version 1.
	SNK_API boost::json::object compose_component_json(const prefab_desc::prefab_comp_node& cnode, const desc::desc_base* parent,
		const desc::desc_system& descs);

	// Whether an override edit at `path` (a JSON pointer) reaches a field of `schema`: the field itself, inside it (an
	// element of a vector) or above it (an object of nested fields). A field key without a leading '/' is a plain key.
	SNK_API bool override_names_a_field(const desc::type_schema& schema, std::string_view path);

	SNK_API void trigger_hot_reload(scn::ecs_assembler& assembler, entt::registry& reg, res::res_handle<desc::desc_base> changed_prefab);
	SNK_API void assemble_prefab(const scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string_view name, res::tag source_prefab);

	// Where one spawn or hot reload is cut short: the project's "prefab_limits" (prj::prefab_spawn_limits, #1031).
	struct spawn_limits
	{
		// Prefab instances open one inside another; plain children and the file a spawn starts from do not count.
		std::size_t nesting_depth = 16;
		std::size_t nodes_per_spawn = 1'000'000;
	};

	// One for the process: scene_init sets the open project's limits (the defaults without a project). Returns the
	// limits it replaced. Not thread-safe: set while nothing spawns.
	SNK_API spawn_limits set_spawn_limits(const spawn_limits& limits);
}