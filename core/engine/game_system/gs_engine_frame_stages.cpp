#include "gs_engine_frame_stages.h"
#include "wnd_window_system.h"
#include "level/scn_level_manager.h"
#include "inp_ecs_input_manager.h"
#include "rnd_frame_assembler.h"
#include "rnd_frame_context.h"
#include "rnd_render_system.h"
#include "app_loop_service_interface.h"
#include "eng_profiler.h"

void gs::register_scene_frame_stages(app::frame_stage_registry& registry)
{
	registry.set_body(app::builtin_stage::update, [](app::stage_context& c) {
		c.storage.require<scn::level_manager>().update(c.dt);
	});
	registry.set_body(app::builtin_stage::post_update, [](app::stage_context& c) {
		if (auto ecs_input = c.storage.require_shared<inp::ecs_input_manager>()) {
			ecs_input->reset_frame_deltas();
		}
	});
}

void gs::register_platform_frame_stages(app::frame_stage_registry& registry)
{
	registry.add_init_hook([](ds::app_data_storage& storage) {
		storage.require<wnd::window_system>().init_windows_frame_time();
	});
	registry.set_body(app::builtin_stage::input, [](app::stage_context& c) {
		PROFILE_SCOPE("PollEvents");
		c.storage.require<wnd::window_system>().pool_events();
	});
	// begin_frame injects the render managers into the frame_context before the extractors read
	// them (EPIC-49 US-49-6).
	registry.set_body(app::builtin_stage::extract, [](app::stage_context& c) {
		auto& frame = c.storage.require<rnd::frame_context>();
		c.storage.require<rnd::render_system>().begin_frame(frame);
		if (const auto window = c.storage.require<wnd::window_system>().get_active_window()) {
			frame.backbuffer_size = window->get_backbuffer_size();
		}
		c.storage.require<rnd::frame_assembler>().build_frame(frame);
	});
	registry.set_body(app::builtin_stage::render, [](app::stage_context& c) {
		c.storage.require<rnd::render_system>().render_frame(c.storage.require<rnd::frame_context>());
	});
	registry.set_body(app::builtin_stage::present, [](app::stage_context& c) {
		PROFILE_SCOPE("SwapBuffers");
		auto& windows = c.storage.require<wnd::window_system>();
		windows.process_windows();
		if (windows.is_stop_running()) {
			c.storage.require<app::app_loop_service_interface>().request_stop();
		}
	});
	// The frame_context carries per-frame data (possibly frame-allocator memory): drop it here,
	// before the next `begin` resets the allocator.
	registry.add_stage("frame_end", app::builtin_stage::present, app::stage_side::after, [](app::stage_context& c) {
		c.storage.require<rnd::frame_context>() = rnd::frame_context{};
	});
}
