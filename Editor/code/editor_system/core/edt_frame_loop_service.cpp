#include "edt_frame_loop_service.h"
#include "rnd_render_system.h"
#include "inp_input_system.h"
#include "inp_ecs_input_manager.h"
#include "ecs_system.h"
#include "wnd_window_system.h"
#include "level/scn_level_manager.h"
#include "level/scn_render_data_extractor.h"
#include "gui_system.h"
#include "rnd_frame_assembler.h"
#include "eng_profiler.h"

edt::edt_loop_service::edt_loop_service()
	: stop_requested(false)
{}

edt::edt_loop_service::~edt_loop_service()
{}

void edt::edt_loop_service::init(ds::app_data_storage& storage)
{
	auto& window_system_ref = storage.require<wnd::window_system>();

	window_system_ref.init_windows_frame_time();
	previous_time = std::chrono::steady_clock::now();

	gs::register_core_frame_stages(m_stages);
	// The editor's ImGui pass is the body of the `ui` stage, between the 3D render and present.
	m_stages.set_body(gs::builtin_stage::ui, [](gs::frame_step_context& c) {
		c.storage.require<gui::gui_system>().render();
	});
}

void edt::edt_loop_service::on_step(ds::app_data_storage& storage)
{
	PROFILE_FRAME("MainThread");
	PROFILE_SCOPE("FrameTotal");

	auto current_time = std::chrono::steady_clock::now();
	std::chrono::duration<float> duration = current_time - previous_time;
	previous_time = current_time;

	rnd::frame_context context;
	gs::frame_step_context ctx{ storage, duration, context };
	m_stages.run(ctx);

	stop_requested = storage.require<wnd::window_system>().is_stop_running();
}

bool edt::edt_loop_service::should_stop() const
{
	return stop_requested;
}