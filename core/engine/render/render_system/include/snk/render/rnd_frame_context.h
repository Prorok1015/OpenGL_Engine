#pragma once
#include <glm/vec2.hpp>
#include <snk/common/ds/ds_store.hpp>

namespace rnd {
	class texture_manager;
	class shader_config_cache;
	struct material_compiler_interface;

	// What the extract stage works with: the frame's data (the packets) and the few services extraction needs.
	// The render passes do not get this; they get a render_context, which has none of these services.
	struct frame_context {
		ds::app_data_storage data;

		// Borrowed, non-owning; valid for one frame only. Injected by render_system::begin_frame
		// before the extract stage runs.
		texture_manager*     textures = nullptr;
		shader_config_cache* variants = nullptr;

		// Render-side material compiler (EPIC-49 US-49-5): the scene extractor turns POD
		// scn::material_desc into rnd::shader_config through this instead of a scene-side method.
		material_compiler_interface* material_compiler = nullptr;

		// Size in pixels of the window's default framebuffer ({0,0} when unknown). A camera that
		// draws to the backbuffer and has no viewport of its own covers all of it.
		glm::ivec2 backbuffer_size{ 0 };
	};
}
