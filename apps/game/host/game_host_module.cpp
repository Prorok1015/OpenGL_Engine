#include "game_host_module.h"
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service_interface.h>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/desc/desc_system.h>
#include <snk/project/prj_project.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_start_level.h>
#if SNK_HAS_ASSIMP
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#endif
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace {
	// Empty: the project's start_scene.
	CFG_VAR_DEF_STRING(cfg_game_start_level, "game/start_level", "");
	// CI: the run ends with 0 right after the start level loaded.
	CFG_VAR_DEF_BOOL(cfg_game_exit_after_level_loaded, "game/exit_after_level_loaded", false);

	constexpr std::string_view k_start_level_stage = "game_start_level";

	struct start_level_state
	{
		std::optional<scn::start_level_loader> loader;
		bool done = false;
	};

	void stop(ds::app_data_storage& data, int exit_code)
	{
		data.require<snk::game_host::run_outcome>().exit_code = exit_code;
		data.require<app::app_loop_service_interface>().request_stop();
	}

	void step_start_level(start_level_state& state, ds::app_data_storage& data)
	{
		if (state.done) {
			return;
		}
		if (!state.loader) {
			// Held back for its engine version: run() reports it and exits with its code.
			if (data.has_value<prj::held_project>()) {
				state.done = true;
				data.require<app::app_loop_service_interface>().request_stop();
				return;
			}
			// On the first frame, not in init: the project's game modules are loaded after the host's own, and the level
			// may use their types.
			const auto level = scn::resolve_start_level(data, *cfg_game_start_level);
			if (!level) {
				egLOG_ERROR("game", "no start level: the project names no start_scene and game/start_level is not set");
				state.done = true;
				stop(data, snk::game_host::k_exit_no_start_level);
				return;
			}
			state.loader.emplace(data.require<scn::context_manager>(), data.require<res::resource_system>());
			state.loader->begin(*level);
		}
		switch (state.loader->poll()) {
		case scn::start_level_loader::state::loaded:
			state.done = true;
			if (cfg_game_exit_after_level_loaded) {
				egLOG_INFO("app", "exit after the level loaded (game/exit_after_level_loaded)");
				data.require<app::app_loop_service_interface>().request_stop();
			}
			break;
		case scn::start_level_loader::state::failed:
			state.done = true;
			stop(data, snk::game_host::k_exit_level_failed);
			break;
		case scn::start_level_loader::state::idle:
		case scn::start_level_loader::state::loading:
			break;
		}
	}
}

void snk::game_host::game_host_module::register_services(ds::app_data_storage& data)
{
	data.construct<run_outcome>();
	// Before `update`: the set "level" is built before the frame steps it.
	auto state = std::make_shared<start_level_state>();
	data.require<app::frame_stage_registry>().add_stage(std::string{ k_start_level_stage }, app::builtin_stage::update,
		app::stage_side::before, [state](app::stage_context& c) { step_start_level(*state, c.storage); });
}

void snk::game_host::game_host_module::initialize_services(ds::app_data_storage& data)
{
#if SNK_HAS_ASSIMP
	// Levels name .glb/.gltf/.obj/.fbx models directly; the editor registers the same adapter for itself.
	auto& resources = data.require<res::resource_system>();
	resources.registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO,
		std::ref(data.require<desc::desc_system>()), std::ref(resources));
#else
	static_cast<void>(data);
	egLOG_WARN("game", "built without assimp (SNK_WITH_ASSIMP=OFF): levels that name models cannot show them");
#endif
}

void snk::game_host::game_host_module::shutdown_services(ds::app_data_storage& data)
{
	data.require<app::frame_stage_registry>().remove_stage(k_start_level_stage);
#if SNK_HAS_ASSIMP
	data.require<res::resource_system>().unregistrate_adapter(scn::model_importer_adapter::INFO);
#endif
	data.destruct<run_outcome>();
}
