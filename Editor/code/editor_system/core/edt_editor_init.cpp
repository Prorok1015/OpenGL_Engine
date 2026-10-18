#include "edt_editor_init.h"
#include "edt_editor_system.h"
#include "desc_system.h"
#include "app_frame_stage.h"
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
		data.require<app::frame_stage_registry>().set_body(app::builtin_stage::ui, [](app::stage_context& c) {
			c.storage.require<gui::gui_system>().render();
		});
	}

	void editor_term(ds::app_data_storage& data)
	{
		data.require<app::frame_stage_registry>().clear_body(app::builtin_stage::ui);
		data.destruct<edt::editor_system>();
	}

}