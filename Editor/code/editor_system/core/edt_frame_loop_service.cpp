#include "edt_frame_loop_service.h"
#include "rnd_render_system.h"
#include "inp_input_system.h"
#include "ecs_system.h"
#include "ecs_common_system.h"
#include "gs_game_system.h"
#include "wnd_window_system.h"
#include "level/scn_level_manager.h"
#include "level/scn_render_data_extractor.h"
#include "gui_system.h"
#include "rnd_frame_assembler.h"

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
}

void edt::edt_loop_service::on_step(ds::app_data_storage& storage)
{
	auto current_time = std::chrono::steady_clock::now();
	std::chrono::duration<float> duration = current_time - previous_time;
	previous_time = current_time;

	auto& window_system_ref = storage.require<wnd::window_system>();
	window_system_ref.pool_events();

	storage.require<scn::level_manager>().update(duration);

	rnd::frame_context context;

	storage.require<rnd::frame_assembler>().build_frame(context);

	storage.require<rnd::render_system>().render_frame(context);

	storage.require<gui::gui_system>().render();

	window_system_ref.process_windows();
	stop_requested = window_system_ref.is_stop_running();
}

bool edt::edt_loop_service::should_stop() const
{
	return stop_requested;
}