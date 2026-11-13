#pragma once
#include <snk/project_migration/pmg_project_steps.h>

// The project steps of EPIC-57 (S57-12, #776) in the registry of EPIC-58: EPIC-57 owns what they do, EPIC-58 runs,
// journals and rolls them back.
namespace prj::migration
{
	// assets.ids (pre): a .resource for every content file without one (scanned_file::has_resource). Its ID is the
	// root __guid of a desc when that is a valid ID no other file holds, else a new one; a __guid another file took
	// first (by path) is in the report. The desc keeps its __guid until S57-8 (#772) stops the editor's saves writing it.
	void register_asset_project_steps(project_step_registry& registry);

	// What a migration command registers: the engine's steps, then the assets' (EPIC-58 3.3: the engine's first).
	void register_project_steps(project_step_registry& registry);
}
