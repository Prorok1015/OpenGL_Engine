#pragma once
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>
#include <snk/scene/level/scn_prefab_desc.h>

namespace scn
{
	class context_manager;

	// The engine's event types of the worlds' buses (WORLD_BUS.md): before the first layer, as add_event requires.
	SNK_API void register_engine_event_types(context_manager& contexts);

	// The open project's prefab_limits (prj::project_context in `store`); the defaults without a project.
	SNK_API spawn_limits project_spawn_limits(const ds::app_data_storage& store);

	// Sets the spawn limits of the open project first, so none stay from an earlier project or test.
	SNK_API void scene_init(ds::app_data_storage& store);
	// The scene layers' and the root's resource loads stop (shutdown, before the scene types are unregistered: #653).
	SNK_API void stop_scene_loads(ds::app_data_storage& store);
	SNK_API void scene_term(ds::app_data_storage& store);
}