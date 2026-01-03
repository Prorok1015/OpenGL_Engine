#include "rnd_render_service_init.h"
#include "rnd_render_system.h"
#include "wnd_window_system.h"
#include "geom/rnd_geometry_desc.h"
#include "texture/rnd_texture_desc.h"

extern rnd::render_system* p_render_system;

void engine::render::render_init(ds::app_data_storage& data)
{
	auto& window_system = data.require<wnd::window_system>();
	auto& desc_sys = data.require<desc::desc_system>();
	p_render_system = &data.construct<rnd::render_system>(window_system.get_context()->create_driver(), desc_sys);
	desc_sys.register_desc2<rnd::geometry_desc>("geometry_desc");
	desc_sys.register_desc2<rnd::texture_desc>("texture_desc");
}

void engine::render::render_term(ds::app_data_storage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	desc_sys.unregister_desc2("geometry_desc");
	desc_sys.unregister_desc2("texture_desc");

	data.destruct<rnd::render_system>();
	p_render_system = nullptr;
}