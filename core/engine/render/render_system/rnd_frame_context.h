#pragma once
#include "ds/ds_store.hpp"

namespace rnd {
	class texture_manager;
	class ShaderManager;
	class geom_manager;
	class skinning_manager;

	struct frame_context {
		ds::app_data_storage data;

		// Borrowed, non-owning; valid for one frame only. Injected by render_system::begin_frame
		// before the extract/render stages run (EPIC-49 US-49-6) — replaces rnd::get_system() in
		// the scene extractor and the render passes.
		texture_manager*  textures = nullptr;
		ShaderManager*    shaders  = nullptr;
		geom_manager*     geometry = nullptr;
		skinning_manager* skinning = nullptr; // nullable (optional subsystem)
	};
}