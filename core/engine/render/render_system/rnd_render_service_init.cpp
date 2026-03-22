#include "rnd_render_service_init.h"
#include "rnd_render_system.h"
#include "wnd_window_system.h"
#include "geom/rnd_geometry_desc.h"
#include "texture/rnd_texture_desc.h"
#include "texture/rnd_texture_2d_desc.h"
#include "texture/rnd_texture_cubemap_desc.h"
#include "passes/rnd_z_prepass.h"
#include "passes/rnd_opaque_pass.h"
#include "passes/rnd_transparent_pass.h"
#include "passes/rnd_composition_pass.h"
#include "passes/rnd_skybox_pass.h"
#include "passes/rnd_debug_pass.h"

extern rnd::render_system* p_render_system;

void engine::render::render_init(ds::app_data_storage& data)
{
	auto& window_system = data.require<wnd::window_system>();
	auto& desc_sys = data.require<desc::desc_system>();
	p_render_system = &data.construct<rnd::render_system>(window_system.get_context()->create_driver(), desc_sys);
	desc_sys.register_desc<rnd::geometry_desc>("geometry_desc");
	desc_sys.register_desc<rnd::texture_desc>("texture_desc");
	desc_sys.register_desc<rnd::texture_2d_desc>("texture_2d_desc");
	desc_sys.register_desc<rnd::texture_cubemap_desc>("texture_cubemap_desc");
	p_render_system->add_render_pass(std::make_shared<rnd::z_prepass>());
	p_render_system->add_render_pass(std::make_shared<rnd::opaque_pass>());
	p_render_system->add_render_pass(std::make_shared<rnd::skybox_pass>());
	p_render_system->add_render_pass(std::make_shared<rnd::transparent_pass>());
	p_render_system->add_render_pass(std::make_shared<rnd::debug_pass>());
	p_render_system->add_render_pass(std::make_shared<rnd::composition_pass>());
}

void engine::render::render_term(ds::app_data_storage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	desc_sys.unregister_desc("texture_cubemap_desc");
	desc_sys.unregister_desc("texture_2d_desc");
	desc_sys.unregister_desc("texture_desc");
	desc_sys.unregister_desc("geometry_desc");

	data.destruct<rnd::render_system>();
	p_render_system = nullptr;
}