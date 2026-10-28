#pragma once
#include <snk/application/app_module_interface.h>
#include <snk/application/app_module_layer.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/scene/scn_scene_layer.h>
#include <snk/common/snk_api.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace scn { class context_manager; }

namespace gs
{
	// Base of a game module (MODULE_LAYER.md): it is layered(), so the module loader gives it a layer, and it fills
	// the layer in register_content. The layer goes away as a whole after shutdown, so nothing registered there needs
	// to be removed by hand.
	//
	// Everything the module adds goes through the layer: systems, phases, descs, spawners, component names, meta
	// (ecs::register_component<T>(layer.meta()), never a binary's entt::locator), message types (layer.channels()).
	// Registrations it makes in a base registry outside the layer (a command, a log sink) are kept by the layer
	// (layer.keep(handle)), and its tasks are bound to layer.tasks(); the layer drops both before it goes. Its own
	// services go into `data` (the layer's storage) and are destroyed after the layer. The base sets are built from
	// the base registries alone and the module's sets go with its layer, so unloading needs no rebuild_all_plans.
	class game_module : public app::modules::module_interface
	{
	public:
		bool layered() const final { return true; }
		std::vector<std::string_view> depends_on() const override { return { "core", "engine" }; }

		void register_services(ds::app_data_storage& data) final { register_content(data.require<scn::scene_layer>(), data); }
		void initialize_services(ds::app_data_storage& data) override { initialize_content(data.require<scn::scene_layer>(), data); }
		void shutdown_services(ds::app_data_storage& data) final { shutdown_content(data.require<scn::scene_layer>(), data); }

	protected:
		virtual void register_content(scn::scene_layer& layer, ds::app_data_storage& data) = 0;
		virtual void initialize_content(scn::scene_layer&, ds::app_data_storage&) {}
		// Stops what runs the module's code outside what the layer drops by itself.
		virtual void shutdown_content(scn::scene_layer&, ds::app_data_storage&) {}
	};

	// The engine's module_layer_factory: while a game module is loaded its layer is the game layer, and a fresh empty
	// one replaces it afterwards. One game module at a time: a second one is refused (std::logic_error) until layers
	// of several modules are designed (#21). Destroying a module's layer goes: its tasks, its worlds, the rest of the
	// layer, then the module's services (MODULE_LAYER.md §6).
	class SNK_API game_layer_factory final : public app::modules::module_layer_factory
	{
	public:
		// `tasks`: what the layers' tasks run on (scn::scene_layer::tasks); null when modules run none.
		explicit game_layer_factory(scn::context_manager& contexts, ds::task_system* tasks = nullptr)
			: m_contexts(contexts), m_tasks(tasks) {}
		std::unique_ptr<app::modules::module_layer> create(ds::app_data_storage& root, std::string_view module) override;

	private:
		scn::context_manager& m_contexts;
		ds::task_system* m_tasks = nullptr;
		// The game module whose layer is the game layer now; empty: none.
		std::string m_current;
	};
}
