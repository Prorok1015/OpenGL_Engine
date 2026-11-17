#pragma once
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema_ops.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <entt/entity/registry.hpp>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ecs { class blackboard_data; }

namespace scn
{
	class blackboard_catalog;
	class component_catalog;
	struct component_entry;
	class world_context;

	// What a snapshot left behind, by catalog name (or entt type name for a type the catalog does not know), with the
	// number of entities (1 for a ctx resource). Never an error: the owner warns and shows it (HOT_RELOAD_PLAY.md 4.4).
	struct snapshot_report
	{
		// Captured: types without a schema, or not in the catalog at all.
		std::map<std::string, std::size_t> unsaved;
		// Restored: types the snapshot has and the catalog no longer knows.
		std::map<std::string, std::size_t> unknown;
		// Restored, but a value did not fit its field (a field changed its type): that field kept its default.
		std::map<std::string, std::size_t> partial;
		// Restored, but the snapshot holds a field the schema no longer has (removed or renamed): its value is dropped.
		std::map<std::string, std::size_t> removed_fields;
		std::size_t entities = 0;
	};

	// The components of one entity as capture_world writes them, so that other readers of a live world (the Play
	// inspector, #552) show the same values the same way. Built once per pass: it collects the catalog's entries, which
	// must outlive it.
	class SNK_API entity_writer
	{
	public:
		struct components
		{
			// By catalog name, as desc JSON through the schema.
			json::object saved;
			// Present, but the type has no schema: its value cannot be shown.
			std::vector<std::string> unsaved;
		};

		explicit entity_writer(const component_catalog& catalog);

		// Carries an editor-only component (the editor's cameras): a snapshot leaves the entity out.
		bool editor_only(const entt::registry& reg, entt::entity entity) const;
		// The catalog names of every component the entity has, with a schema or without.
		std::vector<std::string> present(const entt::registry& reg, entt::entity entity) const;
		// `only`: just these names (null: all).
		components write(const entt::registry& reg, entt::entity entity, const std::vector<std::string>* only = nullptr) const;

	private:
		std::vector<const component_entry*> m_data;
		std::vector<const component_entry*> m_editor_only;
	};

	// The live state of one world as plain data (HOT_RELOAD_PLAY.md section 4.1): every entity by its raw id, the
	// components and ctx resources the catalog saves, world_time, the fixed step not yet taken and the random stream.
	// Entities carrying an editor-only component (the editor's cameras) are left out. Main thread, between frames.
	SNK_API json::object capture_world(const world_context& world, const component_catalog& catalog, snapshot_report& report);

	// Rebuilds a world from capture_world into `world`, whose registry must hold no entity. Entities are created first
	// (with their node path, so node_index is whole), then their components in catalog order with references mapped to
	// the new entities; derived components (world transform, depth, bone matrices) are rebuilt and the transform
	// events raised so that the first frame computes them. `context.system` (the desc system of the world's layer) is
	// what reads desc references and inline descs: without it materials, clips and remembered prefab nodes read back
	// empty; without `context.owner` an owner named after the world stands in. Its `entities` is replaced. Handles
	// come back loading: waiting for them is the caller's (HOT_RELOAD_PLAY.md section 6, step 6). False, with the
	// world left partly built, when the snapshot is not one.
	SNK_API bool restore_world(world_context& world, const json::object& snapshot, const component_catalog& catalog,
		const desc::read_context& context, snapshot_report& report);

	// The committed blackboard of a set as [{key, type, value}], each value through the schema `types` has for it. A
	// value of a type without one is left out and counted in report.unsaved under entt's type name (HOT_RELOAD_PLAY.md
	// 4.4: lost, with a warning from the owner).
	SNK_API json::array capture_blackboard(const ecs::blackboard_data& board, const blackboard_catalog& types, snapshot_report& report);

	// Puts capture_blackboard's values back as committed. A type `types` no longer knows goes to report.unknown and is
	// skipped; a value that does not fit its field to report.partial (the field keeps its default).
	SNK_API void restore_blackboard(ecs::blackboard_data& board, const json::array& values, const blackboard_catalog& types,
		const desc::read_context& context, snapshot_report& report);
}
