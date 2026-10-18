#include "edt_editor_init.h"
#include "edt_editor_system.h"
#include "desc_system.h"
#include <snk/application/app_frame_stage.h>
#include "edt_camera_redirect.h"
#include "level/scn_level_manager.h"
#include "rnd_render_system.h"
#include "gui_system.h"
#include "res_system.h"

namespace components {

	void editor_init(ds::app_data_storage& data)
	{
		auto& desc_system = data.require<desc::desc_system>();
		auto& res_sys     = data.require<res::resource_system>();
		auto& rnd_sys     = data.require<rnd::render_system>();
		auto& gui_sys     = data.require<gui::gui_system>();
		data.construct<edt::editor_system>(desc_system, res_sys, rnd_sys, gui_sys);
		// The editor's ImGui pass is the body of the `ui` stage, between the 3D render and present.
		auto& stages = data.require<app::frame_stage_registry>();
		stages.set_body(app::builtin_stage::ui, [](app::stage_context& c) {
			c.storage.require<gui::gui_system>().render();
		});
		// The 3D view is shown inside an ImGui panel: game cameras draw into textures, not the backbuffer.
		// The redirector also releases the textures of cameras that no longer exist.
		auto redirector = std::make_shared<edt::camera_redirector>();
		stages.add_stage("editor_redirect_cameras", app::builtin_stage::extract, app::stage_side::before,
			[redirector](app::stage_context& c) {
				auto& textures = c.storage.require<rnd::render_system>().get_texture_manager();
				redirector->update(c.storage.require<scn::level_manager>().get_level(),
					[&textures](const res::tag& tag) { textures.remove(tag); });
			});
	}

	void editor_term(ds::app_data_storage& data)
	{
		auto& stages = data.require<app::frame_stage_registry>();
		stages.remove_stage("editor_redirect_cameras");
		stages.clear_body(app::builtin_stage::ui);
		data.destruct<edt::editor_system>();
	}

}