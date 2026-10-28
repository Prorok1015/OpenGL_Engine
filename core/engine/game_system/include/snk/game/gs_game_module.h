#pragma once
#include <snk/application/app_module_interface.h>
#include <snk/application/app_module_layer.h>
#include <snk/command/cmd_types.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/config/cfg_api.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/common/snk_api.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace scn { class context_manager; }
namespace cmd { class command_registry; }

namespace gs
{
	// Base of a game module (MODULE_LAYER.md): it is layered(), so the module loader gives it a layer, and it fills
	// the layer in register_content. The layer goes away as a whole after shutdown, so nothing registered there needs
	// to be removed by hand.
	//
	// Everything the module adds goes through the layer: systems, phases, descs, spawners, component names, meta
	// (ecs::register_component<T>(layer.meta()), never a binary's entt::locator), message types (layer.channels()).
	// `data` (the layer's storage) is isolated: the base's registries are not handed out, and what the module registers
	// in the engine outside the layer (commands, log sinks, tasks) goes through gs::module_context, which the layer
	// keeps and removes before it goes. Its own services go into `data` and are destroyed after the layer. The base
	// sets are built from the base registries alone and the module's sets go with its layer, so unloading needs no
	// rebuild_all_plans.
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

	// Where a game module's layer may store and erase resources: memory://modules/<module>/ (MODULE_LAYER.md). Any
	// other tag, res:// included, is refused with std::logic_error.
	SNK_API std::string module_resource_prefix(std::string_view module);

	// What a game module registers in the engine outside its layer (MODULE_LAYER.md §5), in its layer's storage
	// (data.require<gs::module_context>()). Every registration is kept by the layer and removed when the layer goes, so
	// the engine holds nothing that refers to the module.
	class SNK_API module_context
	{
	public:
		module_context(scn::scene_layer& layer, cmd::command_registry* commands, ds::task_system* tasks)
			: m_layer(layer), m_commands(commands), m_tasks(tasks) {}

		// A command in the process-wide registry; false when there is none or it refused the command (logged).
		bool add_command(cmd::command_desc desc);
		// A listener of the engine log; the only way a module gets one (installing one directly is base-only).
		void add_log_sink(engine::log_callback_t callback);
		// The configuration, read-only: defining or writing a variable is base-only.
		const cfg::config_view& config() const { return m_config; }
		// A task bound to the layer: cancelled and waited for before the layer's worlds go. Not a template: the task's
		// state is made by the engine, not by the module's code, whose control block and vtable would otherwise stay
		// in the engine's queues after the module is gone (#406). A result goes back through what `fn` captures.
		ds::task_handle<void> run(ds::lane lane, ds::task_desc desc, std::function<void(ds::task_context&)> fn);

	private:
		scn::scene_layer& m_layer;
		cmd::command_registry* m_commands = nullptr;
		ds::task_system* m_tasks = nullptr;
		cfg::config_view m_config;
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
