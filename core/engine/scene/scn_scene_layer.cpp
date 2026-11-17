#include <snk/scene/scn_scene_layer.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/scene/adapters/scn_worldwrap_adapter.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <functional>

scn::scene_layer::scene_layer(res::resource_system& base_resources, ecs_assembler& base_assembler, ecs::system_factory& base_systems,
	const ecs::channel_registry& base_channels, const ecs::bus_registry& base_events, const component_catalog& base_catalog,
	ds::task_system* tasks)
	: m_resources(std::make_unique<res::resource_system>(base_resources))
	, m_descs(std::make_unique<desc::desc_system>(*m_resources, &base_assembler.get_desc_system()))
	, m_assembler(*m_descs, *m_resources, &base_assembler)
	, m_systems(&base_systems)
	, m_catalog(&base_catalog)
{
	// The layer's descs (and worlds) are created by the layer's desc system, so they live in the layer's cache.
	m_resources->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*m_resources), std::ref(*m_descs));
	m_resources->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*m_resources), std::ref(*m_descs));
	m_hot_reload = std::make_shared<hot_reload_manager>(m_assembler, *m_resources);
	m_channels = std::make_shared<ecs::channel_registry>(&base_channels);
	m_events = std::make_shared<ecs::bus_registry>(&base_events);
	m_task_system = tasks;
}

scn::scene_layer::~scene_layer()
{
	// A load of the layer reads its desc registrations and schemas: the loads end before anything of the layer goes (#988).
	m_resources->stop_loading();
	// The module's catalog entries hold functions and schema deleters of its binary: they go first.
	m_catalog.clear();
	m_blackboard_types.clear();
	stop_tasks();
	while (!m_kept.empty()) {
		m_kept.pop_back();
	}
	m_hot_reload.reset();
	m_resources.reset();
}

void scn::scene_layer::stop_tasks()
{
	m_tasks.refuse_new_tasks();
	m_tasks.cancel_tasks();
	if (m_task_system) {
		m_task_system->wait_idle(m_tasks);
	}
}

res::resource_system& scn::scene_layer::resources()
{
	return *m_resources;
}

desc::desc_system& scn::scene_layer::descs()
{
	return *m_descs;
}

scn::ecs_assembler& scn::scene_layer::assembler()
{
	return m_assembler;
}

ecs::system_factory& scn::scene_layer::systems()
{
	return m_systems;
}

ecs::component_registry& scn::scene_layer::components()
{
	return m_components;
}

scn::component_catalog& scn::scene_layer::catalog()
{
	return m_catalog;
}

const scn::component_catalog& scn::scene_layer::catalog() const
{
	return m_catalog;
}

entt::meta_ctx& scn::scene_layer::meta()
{
	return m_meta;
}

const entt::meta_ctx& scn::scene_layer::meta() const
{
	return m_meta;
}

const std::shared_ptr<scn::hot_reload_manager>& scn::scene_layer::hot_reload() const
{
	return m_hot_reload;
}

ecs::channel_registry& scn::scene_layer::channels()
{
	return *m_channels;
}

const std::shared_ptr<ecs::channel_registry>& scn::scene_layer::channel_types() const
{
	return m_channels;
}

ecs::bus_registry& scn::scene_layer::events()
{
	return *m_events;
}

const std::shared_ptr<ecs::bus_registry>& scn::scene_layer::event_types() const
{
	return m_events;
}

scn::event_subscriptions& scn::scene_layer::subscriptions()
{
	return m_subscriptions;
}

scn::blackboard_catalog& scn::scene_layer::blackboard_types()
{
	return m_blackboard_types;
}

const scn::blackboard_catalog& scn::scene_layer::blackboard_types() const
{
	return m_blackboard_types;
}

const ds::lifetime& scn::scene_layer::tasks() const
{
	return m_tasks;
}

ds::task_system* scn::scene_layer::task_runner() const
{
	return m_task_system;
}

void scn::scene_layer::keep_erased(std::shared_ptr<void> object)
{
	m_kept.push_back(std::move(object));
}

std::size_t scn::scene_layer::kept_count() const
{
	return m_kept.size();
}
