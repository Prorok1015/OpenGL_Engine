#pragma once
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>

namespace scn
{
	class context_manager;

	// The engine's event types of the worlds' buses (WORLD_BUS.md): before the first layer, as add_event requires.
	SNK_API void register_engine_event_types(context_manager& contexts);

	SNK_API void scene_init(ds::app_data_storage& store);
	// The scene layers' and the root's resource loads stop (shutdown, before the scene types are unregistered: #653).
	SNK_API void stop_scene_loads(ds::app_data_storage& store);
	SNK_API void scene_term(ds::app_data_storage& store);
}