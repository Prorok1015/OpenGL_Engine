#include "editor_module.h"
#include "edt_editor_init.h"
#include "res_system.h"
#include "resources/res_resource_picture.h"
#include "wnd_window_system.h"
#include "inp_input_system.h"
#include "edt_editor_system.h"
#include "res_file_watcher.h"
#include "cfg_api.h"

CFG_VAR_EXT_PATH(cfg_res_path);

void editor::editor_module::register_services(ds::app_data_storage& data)
{
	using namespace components;
	//4
	editor_init(data);// editor
}

void editor::editor_module::initialize_services(ds::app_data_storage& data)
{
	// Initialize editor services here

	auto& res = data.require<res::resource_system>();
	auto& win_service = data.require<wnd::window_system>();

	auto logo = res.require_resource<res::picture_resource>(res::tag::make("icons/editor_engine_logo.png")).get_sync();
	ds::fixed_vector<wnd::window_image, 2> images;
	images.emplace_back(logo->data(), logo->size().x, logo->size().y, wnd::window_image::TYPE::LOGO);
	win_service.get_active_window()->set_logo(images);
	win_service.get_active_window()->set_title("Snake Editor");

	data.require<edt::editor_system>().init(data.require<inp::input_system>());
}

void editor::editor_module::shutdown_services(ds::app_data_storage& data)
{
	using namespace components;
	editor_term(data);
}