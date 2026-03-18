#include "edt_editor_init.h"
#include "edt_editor_system.h"
#include "desc_system.h"
#include "edt_frame_loop_service.h"
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
		data.construct<app::app_loop_service_interface, edt::edt_loop_service>();
	}

	void editor_term(ds::app_data_storage& data)
	{
		data.destruct<app::app_loop_service_interface>();
		data.destruct<edt::editor_system>();
	}

}