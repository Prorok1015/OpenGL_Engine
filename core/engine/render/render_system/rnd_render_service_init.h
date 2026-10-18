#pragma once
#include "ds/ds_store.hpp"
#include "snk_api.h"

namespace desc {
	class desc_system;
}

namespace engine::render {
	// The descs the renderer owns (geometry, textures). Needed wherever such files are loaded without a render system.
	SNK_API void register_render_descs(desc::desc_system& desc_sys);
	SNK_API void unregister_render_descs(desc::desc_system& desc_sys);

	void render_init(ds::app_data_storage& data);
	void render_term(ds::app_data_storage& data);
}