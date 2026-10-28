#pragma once
#include <snk/desc/desc_system.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_component_registry.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/common/snk_api.h>
#include <entt/meta/context.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace scn
{
	class hot_reload_manager;

	// The services of a layer over the base (MODULE_LAYER.md): a resource layer with a desc adapter on the layer's
	// desc system, desc system, assembler, system factory and channel types over the base ones, component names, a
	// meta context and the hot reload of the layer's worlds. What a game module registers goes here; the worlds of the
	// layer are the sets the context_manager created for it. Main thread, between frames; the base services outlive it.
	//
	// What a module registers in a base registry outside the layer (a command, a log sink, ...) is kept here by its
	// RAII handle (keep), and its tasks are bound to tasks(): the layer drops both before its services go, and its
	// worlds go before that (context_manager::destroy_layer), so none of the module's code runs once the layer is gone.
	class SNK_API scene_layer
	{
	public:
		// `tasks`: the task system the layer's tasks run on; null when nothing is bound to tasks().
		scene_layer(res::resource_system& base_resources, ecs_assembler& base_assembler, ecs::system_factory& base_systems,
			const ecs::channel_registry& base_channels, ds::task_system* tasks);
		// Stops the layer's tasks, drops what it keeps, then stops the layer's loads before the desc system their
		// adapters use goes away.
		~scene_layer();
		scene_layer(const scene_layer&) = delete;
		scene_layer& operator=(const scene_layer&) = delete;
		scene_layer(scene_layer&&) = delete;
		scene_layer& operator=(scene_layer&&) = delete;

		res::resource_system& resources() { return *m_resources; }
		desc::desc_system& descs() { return m_descs; }
		ecs_assembler& assembler() { return m_assembler; }
		ecs::system_factory& systems() { return m_systems; }
		ecs::component_registry& components() { return m_components; }
		// Never attached to a binary's entt::locator: module code passes it explicitly (ecs::register_component(meta()),
		// entt::resolve(meta(), id)), and the engine finds it through ecs::find_meta.
		entt::meta_ctx& meta() { return m_meta; }
		const entt::meta_ctx& meta() const { return m_meta; }
		const std::shared_ptr<hot_reload_manager>& hot_reload() const { return m_hot_reload; }
		// The message types of the layer's sets: the base's, then the module's (`channels().add<my_message>()`).
		ecs::channel_registry& channels() { return *m_channels; }
		const std::shared_ptr<ecs::channel_registry>& channel_types() const { return m_channels; }

		// Owner of the module's tasks: run them with task_system::run(lane, desc, layer.tasks(), fn).
		const ds::lifetime& tasks() const { return m_tasks; }
		// Cancels the layer's tasks and waits for them. Idempotent; the layer does it before its worlds and services go.
		void stop_tasks();

		// Holds a registration made in a base registry (cmd::registration, engine::log_sink_handle, ...) until the
		// layer goes; what is kept is dropped in reverse order. Returns the kept object.
		template<class T>
		T& keep(T handle)
		{
			auto held = std::make_shared<T>(std::move(handle));
			T& ref = *held;
			m_kept.push_back(std::move(held));
			return ref;
		}
		std::size_t kept_count() const { return m_kept.size(); }

	private:
		entt::meta_ctx m_meta;
		std::unique_ptr<res::resource_system> m_resources;
		desc::desc_system m_descs;
		ecs_assembler m_assembler;
		ecs::system_factory m_systems;
		ecs::component_registry m_components;
		std::shared_ptr<hot_reload_manager> m_hot_reload;
		std::shared_ptr<ecs::channel_registry> m_channels;
		std::vector<std::shared_ptr<void>> m_kept;
		ds::task_system* m_task_system = nullptr;
		ds::lifetime m_tasks{ ds::lifetime_option::cancel_on_destroy };
	};
}
