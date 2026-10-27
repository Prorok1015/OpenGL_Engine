#pragma once
#include "test_scene_env.hpp"
#include <snk/application/app_module_layer.h>
#include <snk/application/app_module_loader.hpp>
#include <snk/common/ds/ds_store.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_game_module.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <memory>

// The engine side a game module is loaded into: the scene services in a root storage, a context_manager and the
// engine's layer factory, as engine_module sets them up (MODULE_LAYER.md).
namespace test_util {

	template<class T>
	std::shared_ptr<T> borrowed(T& object)
	{
		return std::shared_ptr<T>(std::shared_ptr<void>{}, &object);
	}

	struct module_env : scene_env {
		module_env()
			: hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		{
			root.attach(borrowed(*rs));
			root.attach(borrowed(*descs));
			root.attach(borrowed(*assembler));
			root.attach(borrowed(sf));
			auto& contexts = root.construct<scn::context_manager>(*rs, *assembler, sf, hot);
			root.construct<app::modules::module_layer_factory, gs::game_layer_factory>(contexts);
		}

		~module_env()
		{
			// A module still loaded goes first: its layer belongs to the context_manager in `root`.
			loader.shutdown_all_services(root);
			root = ds::app_data_storage{};
		}

		scn::context_manager& contexts() { return root.require<scn::context_manager>(); }

		std::shared_ptr<scn::hot_reload_manager> hot;
		ecs::system_factory sf;
		ds::app_data_storage root;
		app::modules::module_loader loader;
	};

}
