#pragma once
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <glm/vec3.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// How the editor turns an edit inside a prefab instance into override records on the instance node.
// `owner` is the node of the edited desc that instantiates the prefab, `target` the node-id path below it.
namespace edt { class scene_document; struct editor_services; }
namespace scn { class world_desc; }
namespace desc { class desc_system; class schema_registry; struct type_schema; }

namespace edt::override_edit
{
	using prefab_node = scn::prefab_desc::prefab_node;

	// The prefab (loaded) that a node instantiates through one of its reference components, or null.
	const scn::prefab_desc* referenced_prefab(const prefab_node& node);

	// The node of the instantiated content that `target` addresses, composed the way reconcile builds it: records of
	// every prefab level on the way are applied, only the owner's own edits of this node are left out. Name, id,
	// placement and components are filled, children are not. Empty if the content has no such node.
	// `file`: the file `owner` is content of (scene_document::world_tag), open for the owner's expansion only (#993, #995).
	std::optional<prefab_node> find_base_node(scn::ecs_assembler& assembler, const prefab_node& owner, std::span<const uint64_t> target,
		const res::tag& file);

	// A record of an instance that changes something the prefab no longer has, for a person: its number among `owner.overrides`
	// and what is gone (a node or component: scn::find_stale_records; a field the component's schema no longer has).
	struct stale_record {
		std::size_t index = 0;
		std::string what;
		// The steps of its type put its value in more than one place: a person decides, "Clear stale overrides" leaves it.
		bool keep_on_clear = false;
	};

	// A node of an instance as the hierarchy lists it: no children, `has_children` says whether it has some.
	struct listed_node {
		prefab_node node;
		bool has_children = false;
	};

	// The children of the node `target` addresses (empty target: the owner itself), composed like find_base_node does,
	// with the owner's own records applied. For an owner these include the owner's own children.
	// `limit` bounds the children that are expanded (each one costs a copy of its prefab content); `total`, if given,
	// receives how many children there are.
	std::vector<listed_node> list_children(scn::ecs_assembler& assembler, const prefab_node& owner, std::span<const uint64_t> target,
		const res::tag& file, std::size_t limit = static_cast<std::size_t>(-1), std::size_t* total = nullptr);

	// Expanding an instance copies prefab content and logs about broken input, so the panels do it once per selection or
	// edit instead of once per frame. Results are kept per (active world, owner node id, target) and dropped as a whole
	// when scene_document::desc_generation moves, so correctness does not depend on anyone calling invalidate().
	// Callers get shared ownership: what they hold stays valid whatever the cache does meanwhile.
	class view_cache
	{
	public:
		// The prefabs are expanded with the assembler of the game layer current at each lookup.
		view_cache(const editor_services& services, const scene_document& state) : m_services(services), m_state(state) {}

		void invalidate();
		std::shared_ptr<const std::optional<prefab_node>> base(const prefab_node& owner, std::span<const uint64_t> target);
		std::shared_ptr<const std::vector<listed_node>> children(const prefab_node& owner, std::span<const uint64_t> target);
		// The records of `owner` that find nothing to change in the prefab any more, kept like the others: the node and component
		// level from scn::find_stale_records, then the field level: a `set` record whose pointer the schema of the component it
		// sets (as the base content defines it) no longer has (desc::check_override_object: silent), judged at the record's path after the
		// steps of that type (scn::migrate_override_record), or as written where the steps cannot follow it. A node that instantiates no prefab has none.
		std::shared_ptr<const std::vector<stale_record>> stale(const prefab_node& owner);

	private:
		template<typename T>
		struct entry {
			int world;
			uint64_t owner_id;
			std::vector<uint64_t> target;
			std::shared_ptr<const T> value;
			// What the owner's prefab was when the value was made (source_of): another one, and the value is made again.
			// Weak: the cache must not keep a resource block alive (a block still held when the resource system goes is leaked).
			std::weak_ptr<const void> source;
		};

		// The block the resource system holds for the prefab `owner` instantiates now: a changed prefab file is another block, while
		// the handle the owner's component keeps stays the old one. Null when the owner instantiates nothing.
		std::shared_ptr<const void> source_of(const prefab_node& owner) const;
		template<typename T>
		static const entry<T>* find_entry(std::vector<entry<T>>& entries, int world, uint64_t owner_id, std::span<const uint64_t> target,
			const std::weak_ptr<const void>& source);

		void sync_generation();
		// The owners are nodes of the active world: its file is theirs (#993).
		res::tag active_file() const;

		const editor_services& m_services;
		const scene_document& m_state;
		uint64_t m_generation = 0;
		std::vector<entry<std::optional<prefab_node>>> m_bases;
		std::vector<entry<std::vector<listed_node>>> m_children;
		std::vector<entry<std::vector<stale_record>>> m_stale;
	};

	// A component as the base content defines it: the referenced desc's data with the component's own values and edits on top.
	boost::json::object component_json(const scn::prefab_desc::prefab_comp_node& comp);

	// What `edited` changes compared to `base`: objects are compared key by key, everything else as a whole.
	boost::json::object diff(const boost::json::object& base, const boost::json::object& edited);

	// `base` with the owner's records for (target, component) applied: what the instance really has.
	boost::json::object effective_component(
		const prefab_node& owner, std::span<const uint64_t> target, const std::string& component, boost::json::object base);

	void effective_transform(
		const prefab_node& owner, std::span<const uint64_t> target, const prefab_node& base,
		glm::vec3& position, glm::vec3& rotation, glm::vec3& scale);

	// Replace the records of (target, component) by one record holding the difference to the base; no difference, no record.
	// With the component's `schema`, a reference field (EPIC-57 section 4.4) that holds a {guid, path} reference, or an
	// inline desc where the base has a reference, gets a record of its own that sets the value there whole (whole_values):
	// merged as part of the component it would mix with the inline desc or the reference of the base.
	void write_component(
		prefab_node& owner, std::span<const uint64_t> target, const std::string& component,
		const boost::json::object& base, const boost::json::object& edited,
		const desc::desc_system* descs = nullptr, const desc::type_schema* schema = nullptr);

	// The pointers of the reference fields of `edited` (a component of `schema`) whose value goes whole, not merged: a
	// {guid, path} reference, or any object where `base` holds one. Read through structures, maps of them and inline
	// descs, by every key a field is read under.
	std::vector<std::string> whole_values(const desc::desc_system& descs, const desc::type_schema& schema,
		const boost::json::value& base, const boost::json::value& edited);

	// The pointer of the first binary_ref field (a range of a mesh's binary file, EPIC-79) that `patch`, a JSON Merge Patch
	// of a component of `schema`, sets, clears or reaches into, through nested objects and structures; empty when none.
	// Such a field is written only by saving the mesh: an edit that touches it is refused whole. A desc file may hold a
	// whole range there (desc::check_override accepts one, as a migration or the mesh save writes it): the file is the
	// mesh's, an override of a scene or prefab is not (#1180). The Play preflight warns of an override that holds one.
	std::string binary_ref_in_patch(const desc::schema_registry& schemas, const desc::type_schema& schema,
		const boost::json::object& patch);

	// `layer` merged onto `into` as a __parent chain and the Inspector's composed view merge a desc's layers: objects key by
	// key, but a value whole_values names (a {guid, path} reference, or any object over one) replaces what is there.
	void merge_layer(const desc::schema_registry& schemas, const desc::type_schema& schema, boost::json::object& into,
		const boost::json::object& layer);

	// Override records have no "__type": the component of the instance a record applies to says what its value is, so a
	// reference set on an instance is written as {guid, path} too (desc_system::write_references, #773). `level_file` is
	// the file form of `worlds` (scn::level_desc::build_json); its worlds follow them node by node. A record of the whole
	// component keeps a path a string where the component holds an object there, since the record is merged into it.
	// `files`: the file of each world (scene_document::world_tags), empty or short when unknown.
	void write_record_references(const desc::desc_system& descs, scn::ecs_assembler& assembler,
		const std::vector<scn::world_desc>& worlds, std::span<const res::tag> files, boost::json::value& level_file);

	void write_transform(
		prefab_node& owner, std::span<const uint64_t> target, const prefab_node& base,
		glm::vec3 position, glm::vec3 rotation, glm::vec3 scale);

	// Moves the value of `record` into `prefab`, the file content the owner instantiates, the way the owner's record would have
	// changed it: a `set` of a field of a node the file holds is written into that node (its transform, or the component's own
	// values); a record below a node that instantiates another prefab, or on a component that node only gets from it, goes to that
	// node as a record of its own (one level, never into the nested file). Returns the reason it cannot be done (empty: done):
	// `remove` and `add_node` records, a node or component the file does not have. `prefab` is changed only when it works.
	std::string fold_record(scn::prefab_desc& prefab, const scn::override_record& record);

	// How many nodes of `worlds` instantiate the prefab `tag` (the level's own nodes, nested instances inside other prefabs excluded).
	std::size_t count_instances(const std::vector<scn::world_desc>& worlds, const res::tag& tag);

	bool has_record(const prefab_node& owner, std::span<const uint64_t> target, const std::string& component);

	// scn::migrate_records on every node below `root` (itself included) that instantiates a prefab: what the editor does to a document
	// it loads, since a step of the file cannot see the type an instance's record edits (#1255). Returns how many records changed;
	// `moved`, if given, receives how many of them changed their path or value (the rest only got the current version).
	std::size_t migrate_instance_records(const scn::ecs_assembler& assembler, prefab_node& root, std::size_t* moved = nullptr);

	// "Reset to base": drops the records of (target, component). True if there were any.
	bool reset(prefab_node& owner, std::span<const uint64_t> target, const std::string& component);
}
