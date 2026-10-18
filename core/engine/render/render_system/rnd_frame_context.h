#pragma once
#include <glm/vec2.hpp>
#include "ds/ds_store.hpp"

namespace rnd {
	class texture_manager;
	class ShaderManager;
	class geom_manager;
	class skinning_manager;
	struct material_compiler_interface;

	struct frame_context {
		ds::app_data_storage data;

		// Borrowed, non-owning; valid for one frame only. Injected by render_system::begin_frame
		// before the extract/render stages run (EPIC-49 US-49-6) — replaces rnd::get_system() in
		// the scene extractor and the render passes.
		texture_manager*  textures = nullptr;
		ShaderManager*    shaders  = nullptr;
		geom_manager*     geometry = nullptr;
		skinning_manager* skinning = nullptr; // nullable (optional subsystem)

		// Render-side material compiler (EPIC-49 US-49-5): the scene extractor turns POD
		// scn::material_desc into rnd::shader_config through this instead of a scene-side method.
		material_compiler_interface* material_compiler = nullptr;

		// Size in pixels of the window's default framebuffer ({0,0} when unknown). A camera that
		// draws to the backbuffer and has no viewport of its own covers all of it.
		glm::ivec2 backbuffer_size{ 0 };
	};
}