#include <snk/render/rnd_render_service_init.h>
#include <snk/render/rnd_render_system.h>
#include <snk/window/wnd_window_system.h>
#include "rnd_gl_swapchain.h"
#include "rnd_null_swapchain.h"
#include <snk/config/cfg_api.h>
#include <snk/common/logger/engine_log.h>
#include <string>
#include <snk/resource/res_system.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include "texture/rnd_texture_2d_desc.h"
#include "texture/rnd_texture_cubemap_desc.h"
#include "passes/rnd_z_prepass.h"
#include "passes/rnd_opaque_pass.h"
#include "passes/rnd_transparent_pass.h"
#include "passes/rnd_composition_pass.h"
#include "passes/rnd_skybox_pass.h"
#include "passes/rnd_debug_pass.h"
#include <snk/render/shader/rnd_material_compiler.h>

namespace {
	// "gl" (default) or "null". A window without a GL surface selects null regardless.
	// Unknown values warn and fall through to the surface check.
	CFG_VAR_DEF_STRING(cfg_render_backend, "render/backend", "gl");
}

void engine::render::register_render_descs(desc::desc_system& desc_sys)
{
	desc_sys.register_desc<rnd::geometry_desc>("geometry_desc");
	desc_sys.register_desc<rnd::texture_desc>("texture_desc");
	desc_sys.register_desc<rnd::texture_2d_desc>("texture_2d_desc");
	desc_sys.register_desc<rnd::texture_cubemap_desc>("texture_cubemap_desc");
}

void engine::render::unregister_render_descs(desc::desc_system& desc_sys)
{
	desc_sys.unregister_desc("texture_cubemap_desc");
	desc_sys.unregister_desc("texture_2d_desc");
	desc_sys.unregister_desc("texture_desc");
	desc_sys.unregister_desc("geometry_desc");
}

void engine::render::render_init(ds::app_data_storage& data)
{
	auto& window_system = data.require<wnd::window_system>();
	auto& desc_sys = data.require<desc::desc_system>();
	auto& res_sys = data.require<res::resource_system>();
	const auto window = window_system.get_active_window();
	wnd::gl_surface* surface = window ? window->gl_surface_ptr() : nullptr;
	const std::string& backend = *cfg_render_backend;
	if (backend != "gl" && backend != "null") {
		egLOG_WARN("render", "Unknown render/backend '{}', expected 'gl' or 'null'", backend);
	}
	// Destroyed in render_term after the render system, before the window (core shutdown).
	rnd::driver::swapchain_interface* swapchain_ptr = nullptr;
	if (backend == "null" || !surface) {
		if (backend == "null") {
			egLOG_INFO("render", "Render backend: null (render/backend = null)");
		} else {
			egLOG_WARN("render", "render/backend is gl but the window has no GL surface; falling back to the null backend");
		}
		swapchain_ptr = &data.construct<rnd::driver::swapchain_interface, rnd::driver::null::swapchain>();
	} else {
		swapchain_ptr = &data.construct<rnd::driver::swapchain_interface, rnd::driver::gl::swapchain>(*surface);
	}
	auto& swapchain = *swapchain_ptr;
	auto& render_system = data.construct<rnd::render_system>(swapchain.create_driver(), desc_sys, res_sys);
	// Per-frame render data shared by the extract and render stages; reset at the end of each frame.
	data.construct<rnd::frame_context>();
	register_render_descs(desc_sys);
	render_system.add_render_pass(std::make_shared<rnd::z_prepass>());
	render_system.add_render_pass(std::make_shared<rnd::opaque_pass>());
	render_system.add_render_pass(std::make_shared<rnd::skybox_pass>());
	render_system.add_render_pass(std::make_shared<rnd::transparent_pass>());
	render_system.add_render_pass(std::make_shared<rnd::debug_pass>(res_sys));
	render_system.add_render_pass(std::make_shared<rnd::composition_pass>());
	render_system.set_material_compiler(std::make_shared<rnd::default_material_compiler>());
}

void engine::render::render_term(ds::app_data_storage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	unregister_render_descs(desc_sys);

	data.destruct<rnd::frame_context>();
	data.destruct<rnd::render_system>();
	data.destruct<rnd::driver::swapchain_interface>();
}