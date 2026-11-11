#pragma once
#include <snk/application/app_module_interface.h>
#include <snk/application/app_module_layer.h>
#include <snk/command/cmd_types.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/config/cfg_api.h>
#include <snk/game/gs_module_layout.h>
#include <snk/game/gs_input_stack_handle.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/common/snk_api.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace scn { class context_manager; }
namespace cmd { class command_registry; }
namespace inp { class input_router; }

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
	// keeps and removes before it goes. Its own services go into `data` and are destroyed after the layer (or by
	// shutdown_content). On shutdown the layer's tasks stop and their main-thread continuations run first, and a task
	// started from one of them is cancelled at once, so none of the module's code runs after shutdown_content or the
	// services it destroys (#579). The base sets are built from the base registries alone and the module's sets go
	// with its layer, so unloading needs no rebuild_all_plans.
	class game_module : public app::modules::module_interface
	{
	public:
		bool layered() const final { return true; }
		std::vector<std::string_view> depends_on() const override { return { "core", "engine" }; }

		void register_services(ds::app_data_storage& data) final { register_content(data.require<scn::scene_layer>(), data); }
		void initialize_services(ds::app_data_storage& data) override { initialize_content(data.require<scn::scene_layer>(), data); }
		void shutdown_services(ds::app_data_storage& data) final
		{
			auto& layer = data.require<scn::scene_layer>();
			layer.stop_tasks();
			ds::detail::run_pending_main(layer.tasks());
			shutdown_content(layer, data);
		}

	protected:
		virtual void register_content(scn::scene_layer& layer, ds::app_data_storage& data) = 0;
		virtual void initialize_content(scn::scene_layer&, ds::app_data_storage&) {}
		// Stops what runs the module's code outside what the layer drops by itself. The layer's tasks and their
		// continuations have already finished, and a task started now is cancelled at once. So the module's tasks must
		// honour cancellation (task_context::stop): the unload waits for them before this, and one that waits for
		// something only this sets would hang it.
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
		// With an input router, the layer keeps a cleanup that takes every input stack layer of the module away when the layer goes
		// (unload, code reload): a module's layers never outlive it in a set that survives it.
		module_context(scn::scene_layer& layer, cmd::command_registry* commands, ds::task_system* tasks,
			inp::input_router* input = nullptr, scn::context_manager* contexts = nullptr);

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

		// A bus subscription (WORLD_BUS.md 3.4) heard in the worlds of the layer's sets that `filter` matches (level, Play,
		// previews; {set, world} names one world, since the Play twin repeats the level's names). Called per event of T
		// on the main thread during the world's delivery; what it emits goes to the next delivery, what it changes
		// through bus_context::changer()/spawner() lands right after the delivery. The layer keeps it: it survives a
		// data reload (desc, level) and goes with the layer (unload, code reload). The returned connection may be
		// disconnected earlier. T must be an event type of the layer (the engine's or layer.events()).
		template <class T>
		ecs::connection& subscribe(scn::event_filter filter, std::function<void(ecs::bus_context&, const ecs::envelope<T>&)> fn)
		{
			ASSERT_MSG(m_layer.events().contains<T>(), "module_context::subscribe: {} is not an event type of the layer", entt::type_name<T>::value());
			return m_layer.keep(m_layer.subscriptions().subscribe<T>(std::move(filter), std::move(fn)));
		}

		// An observer of component T (module API 3.4): `fn` runs on `what` (construct, update, destroy) of T in the worlds
		// of the layer's sets that `filter` matches. It is connected to a world when the world is created (to one that
		// already exists, at its next update), and like a bus subscriber it hears nothing while the set is in Edit (a
		// level load, an editor edit). Kept by the layer like a subscription. A hot reload in Play (HOT_RELOAD_PLAY.md 6) puts back entities that already existed with these
		// observers muted, so a counter they keep is not counted twice. Connecting to registry.on_construct<T>() directly
		// is not muted and outlives nothing safely: use this instead.
		template <class T>
		ecs::connection& observe(scn::event_filter filter, scn::observed what, std::function<void(ecs::bus_context&, entt::entity)> fn)
		{
			return m_layer.keep(m_layer.subscriptions().observe<T>(std::move(filter), what, std::move(fn)));
		}

		// A component without a C++ type, described by `schema` alone (#606): its desc type, spawner, name and catalog
		// entry go into the layer, which keeps the registration until it goes (the component's pools in the layer's
		// worlds are cleared then). False when it is refused (logged): no component schema, a taken name or id, a
		// schema that cannot be bound.
		bool register_component(desc::type_schema schema);

		// The input stack of `set` (module API 3.7). A set that does not exist (`"play"` while Play does not run) gives an empty
		// handle and an error in the log: the stack lives and dies with its set, the file of the level holds the defaults.
		input_stack_handle input_stack(std::string_view set) const;

	private:
		scn::scene_layer& m_layer;
		cmd::command_registry* m_commands = nullptr;
		ds::task_system* m_tasks = nullptr;
		cfg::config_view m_config;
		inp::input_router* m_input = nullptr;
		scn::context_manager* m_contexts = nullptr;
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
