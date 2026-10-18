#pragma once
#include <snk/application/app_frame_stage.h>
#include <snk/common/snk_api.h>

namespace gs
{
	// Bodies of the built-in stages that need no window and no GPU: level/ECS update and the
	// input-delta reset. Safe to run in a process without a graphics context.
	SNK_API void register_scene_frame_stages(app::frame_stage_registry& registry);

	// Bodies that talk to the window system and the renderer: event polling, render data
	// extraction, the 3D passes, buffer swap, plus the end-of-frame cleanup of the render
	// frame_context. Needs rnd::render_system, rnd::frame_assembler, rnd::frame_context and
	// wnd::window_system in the storage when a frame runs.
	SNK_API void register_platform_frame_stages(app::frame_stage_registry& registry);
}
