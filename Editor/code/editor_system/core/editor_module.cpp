#include <snk/editor/editor_module.h>
#include "edt_editor_init.h"
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_picture.h>
#include <snk/window/wnd_window_system.h>
#include <snk/input/inp_input_system.h>
#include "edt_editor_system.h"
#include "edt_data_protocol.h"
#include <snk/config/cfg_api.h>
#if SNK_HAS_ASSIMP
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#endif
#include <snk/desc/desc_system.h>

#include <snk/render/rnd_frame_assembler.h>
#include <snk/scene/level/scn_render_data_extractor.h>
#include <snk/render/skinning/rnd_skinning_manager.h>

CFG_VAR_EXT_PATH(cfg_res_path);
CFG_VAR_DEF_PATH(cfg_edt_data_path, "editor/data_path", "../data/");
CFG_VAR_DEF_PATH(cfg_edt_user_data_path, "editor/user_data_path", "");

void editor::editor_module::register_services(ds::app_data_storage& data)
{
	using namespace components;
	//4
	editor_init(data);// editor
}

void editor::editor_module::initialize_services(ds::app_data_storage& data)
{
	auto& res = data.require<res::resource_system>();
	auto& win_service = data.require<wnd::window_system>();

	const auto user_data_path = cfg_edt_user_data_path->empty() ? edt::default_user_data_dir() : std::filesystem::path{ *cfg_edt_user_data_path };
	edt::register_data_protocol(res, cfg_edt_data_path, user_data_path);

#if SNK_HAS_ASSIMP
	auto& desc_system = data.require<desc::desc_system>();
	res.registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(desc_system), std::ref(res));
#else
	egLOG_WARN("Editor/Import", "built without assimp (SNK_WITH_ASSIMP=OFF): .glb/.gltf/.obj/.fbx models cannot be loaded or imported");
#endif

	auto logo = res.require_sync<res::picture_resource>(res::tag::make("icons/editor_engine_logo.png"));
	// A missing logo is cosmetic: keep starting the editor without it.
	if (const auto logo_res = logo.get_sync(); ENGINE_VERIFY(logo_res, "editor logo failed to load, window icon skipped")) {
		ds::fixed_vector<wnd::window_image, 2> images;
		images.emplace_back(logo_res->data(), logo_res->size().x, logo_res->size().y, wnd::window_image::TYPE::LOGO);
		win_service.get_active_window()->set_logo(images);
	}
	win_service.get_active_window()->set_title("Snake Editor");

	data.require<edt::editor_system>().init(data);
}

void editor::editor_module::shutdown_services(ds::app_data_storage& data)
{
	using namespace components;
	editor_term(data);
#if SNK_HAS_ASSIMP
	data.require<res::resource_system>().unregistrate_adapter(scn::model_importer_adapter::INFO);
#endif
}