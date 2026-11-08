#pragma once
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema_ops.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <map>
#include <string>

namespace scn
{
	class component_catalog;
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
		std::size_t entities = 0;
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
}
