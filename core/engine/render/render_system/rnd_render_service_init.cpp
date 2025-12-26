#include "rnd_render_service_init.h"
#include "rnd_render_system.h"
#include "wnd_window_system.h"

extern rnd::render_system* p_render_system;

void engine::render::render_init(ds::app_data_storage& data)
{
	auto& window_system = data.require<wnd::window_system>();
	auto& desc_sys = data.require<desc::desc_system>();
	p_render_system = &data.construct<rnd::render_system>(window_system.get_context()->create_driver(), desc_sys);
}

void engine::render::render_term(ds::app_data_storage& data)
{
	data.destruct<rnd::render_system>();
	p_render_system = nullptr;
}