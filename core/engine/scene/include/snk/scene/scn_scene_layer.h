#pragma once
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_component_registry.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/common/snk_api.h>
#include <entt/locator/locator.hpp>
#include <entt/meta/context.hpp>
#include <memory>

namespace scn
{
	class hot_reload_manager;

	// The services of a layer over the base (MODULE_LAYER.md): a resource layer with a desc adapter on the layer's
	// desc system, desc system, assembler and system factory over the base ones, component names, a meta context
	// and the hot reload of the layer's worlds. What a game module registers goes here; the worlds of the layer are
	// the sets the context_manager created for it. Main thread, between frames; the base services outlive it.
	class SNK_API scene_layer
	{
	public:
		scene_layer(res::resource_system& base_resources, ecs_assembler& base_assembler, ecs::system_factory& base_systems);
		// Stops the layer's loads before the desc system their adapters use goes away.
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
		entt::meta_ctx& meta() { return *m_meta.context; }
		const entt::meta_ctx& meta() const { return *m_meta.context; }
		// For entt::locator<entt::meta_ctx>::reset in a module binary.
		const entt::locator<entt::meta_ctx>::node_type& meta_handle() const { return m_meta.handle; }
		const std::shared_ptr<hot_reload_manager>& hot_reload() const { return m_hot_reload; }

	private:
		ecs::meta_context_owner m_meta;
		std::unique_ptr<res::resource_system> m_resources;
		desc::desc_system m_descs;
		ecs_assembler m_assembler;
		ecs::system_factory m_systems;
		ecs::component_registry m_components;
		std::shared_ptr<hot_reload_manager> m_hot_reload;
	};
}
