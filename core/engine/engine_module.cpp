#include "engine_module.h"
#include "rnd_render_service_init.h"
#include "gui_gui_service_init.h"
#include "inp_input_service_init.h"
#include "inp_input_system.h"
#include "wnd_window_system.h"
#include "scn_scene_service_init.h"
#include "scn_ecs_assembler.h"
#include "skinning/rnd_skinning_manager.h"
#include "app_frame_stage.h"
#include "gs_engine_frame_stages.h"

void engine::engine_module::register_services(ds::app_data_storage& data)
{
	input::input_reg(data);// engine
	render::render_init(data);// engine
	gui::gui_init(data);//engine
	scn::scene_init(data);//engine
	auto& stages = data.require<app::frame_stage_registry>();
	gs::register_scene_frame_stages(stages);
	gs::register_platform_frame_stages(stages);
	m_scene_types.emplace(scn::register_scene_types(
		data.require<desc::desc_system>(),
		data.require<scn::ecs_assembler>(),
		data.require<rnd::skinning_manager>()));
}

void engine::engine_module::initialize_services(ds::app_data_storage& data)
{
	auto input_service = data.require_shared<inp::input_system>();
	auto& win_service = data.require<wnd::window_system>();
	if (input_service) {
		win_service.add_event_listener(input_service);
		// HACK! first init window size in input system. 
		// Because window created in window system constructor before input system subscribe
		auto window = win_service.get_active_window();
		input_service->on_window_created(window.get());
		input_service->on_window_resize(window.get(), window->get_size().x, window->get_size().y);
	}
	input::input_init(data);
}

void engine::engine_module::shutdown_services(ds::app_data_storage& data)
{
	auto input_service = data.require_shared<inp::input_system>();
	if (input_service) {
		auto& win_service = data.require<wnd::window_system>();
		win_service.remove_event_listener(input_service);
	}

	m_scene_types.reset();
	scn::scene_term(data);
	gui::gui_term(data);
	render::render_term(data);
	input::input_term(data);
}