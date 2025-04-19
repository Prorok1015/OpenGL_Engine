#include "rnd_render_init.h"
#include "rnd_render_system.h"
#include <wnd_window_system.h>

extern rnd::RenderSystem* p_render_system;

void components::render_init(ds::AppDataStorage& data)
{
	auto& window_system = data.require<wnd::WindowSystem>();
	auto& desc_sys = data.require<desc::desc_system>();
	p_render_system = &data.construct<rnd::RenderSystem>(window_system.get_context()->create_driver(), desc_sys);
}

void components::render_term(ds::AppDataStorage& data)
{
	data.destruct<rnd::RenderSystem>();
	p_render_system = nullptr;
}