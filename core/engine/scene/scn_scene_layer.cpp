#include <snk/scene/scn_scene_layer.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/scene/adapters/scn_worldwrap_adapter.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <functional>

scn::scene_layer::scene_layer(res::resource_system& base_resources, ecs_assembler& base_assembler, ecs::system_factory& base_systems)
	: m_resources(std::make_unique<res::resource_system>(base_resources))
	, m_descs(*m_resources, &base_assembler.get_desc_system())
	, m_assembler(m_descs, *m_resources, &base_assembler)
	, m_systems(&base_systems)
{
	// The layer's descs (and worlds) are created by the layer's desc system, so they live in the layer's cache.
	m_resources->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*m_resources), std::ref(m_descs));
	m_resources->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*m_resources), std::ref(m_descs));
	m_hot_reload = std::make_shared<hot_reload_manager>(m_assembler, *m_resources);
}

scn::scene_layer::~scene_layer()
{
	m_hot_reload.reset();
	m_resources.reset();
}
