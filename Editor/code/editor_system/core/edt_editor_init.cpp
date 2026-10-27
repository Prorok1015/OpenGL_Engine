#include "edt_editor_init.h"
#include "edt_editor_system.h"
#include <snk/editor/edt_editor_context.h>
#include <snk/desc/desc_system.h>
#include <snk/application/app_frame_stage.h>
#include "edt_camera_redirect.h"
#include <snk/scene/level/scn_context_manager.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/gui/gui_system.h>
#include <snk/resource/res_system.h>

namespace components {

	void editor_init(ds::app_data_storage& data)
	{
		// The editor edits the game worlds, so it works with the services of their layer (MODULE_LAYER.md).
		auto* game = data.require<scn::context_manager>().game_layer();
		auto& desc_system = game ? game->descs() : data.require<desc::desc_system>();
		auto& res_sys     = game ? game->resources() : data.require<res::resource_system>();
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
		// Job progress is polled once per frame, after the `tasks` stage delivered the jobs that finished.
		stages.add_stage("editor_jobs", app::builtin_stage::input, app::stage_side::after, [](app::stage_context& c) {
			if (c.storage.has_value<edt::editor_context>()) c.storage.require<edt::editor_context>().jobs().pump();
		});
		auto redirector = std::make_shared<edt::camera_redirector>();
		stages.add_stage("editor_redirect_cameras", app::builtin_stage::extract, app::stage_side::before,
			[redirector](app::stage_context& c) {
				auto& textures = c.storage.require<rnd::render_system>().get_texture_manager();
				redirector->update(c.storage.require<scn::context_manager>(),
					[&textures](const res::tag& tag) { textures.remove(tag); });
			});
	}

	void editor_term(ds::app_data_storage& data)
	{
		auto& stages = data.require<app::frame_stage_registry>();
		stages.remove_stage("editor_redirect_cameras");
		stages.remove_stage("editor_jobs");
		stages.clear_body(app::builtin_stage::ui);
		// Jobs run code owned by editor_system; stop them while it, the context and the task system are all alive.
		if (data.has_value<edt::editor_context>()) {
			data.require<edt::editor_context>().jobs().cancel_all_and_wait();
		}
		// Order matters: ~editor_system releases the scene document (and closes the preview) in the context's document_manager.
		data.destruct<edt::editor_system>();
		// Registered by editor_system::init, after the system itself.
		if (data.has_value<edt::editor_context>()) {
			data.destruct<edt::editor_context>();
		}
	}

}