#include "scn_level.h"
#include "scn_model.h"
#include "ecs_command_buffer.hpp"
#include "ecs_entity_changer.hpp"
#include "ecs_component.h"
#include <entt/fwd.hpp>

scn::level::level(std::shared_ptr<scn::hot_reload_manager> hot_manager)
	: m_hot_reload_manager(std::move(hot_manager))
{
    m_level_state.ctx().emplace<ecs::runtime_context_provider>(
        ecs::runtime_context_provider::getter_type{
        [this](size_t world_id) { return this->create_runtime_context(world_id); }
        });
}

scn::level::~level()
{
    clear();
}

void scn::level::update(std::chrono::duration<float> dt)
{
    m_accumulator += dt.count();

    while (m_accumulator >= m_fixed_step) {
        m_level_state.ctx().insert_or_assign<scn::fixed_time>({ m_fixed_step });

        run_graph(m_fixed_graph);

        m_accumulator -= m_fixed_step;
    }

    m_level_state.ctx().insert_or_assign<delta_time>({ dt.count() });

    float alpha = m_accumulator / m_fixed_step;
    m_level_state.ctx().insert_or_assign<scn::update_alpha>({ alpha });

    run_graph(m_variable_graph);
    sync_point();
}

scn::world& scn::level::create_world(const std::string_view type, uint32_t world_id)
{
    auto& w = *(m_worlds[std::string{ type }] = std::make_unique<scn::world>(world_id, type));
    m_worlds_by_id[world_id] = &w; // Register in ID map

    // Set the WorldID in the world's own registry context
    w.state().ctx().emplace<ecs::world_salt>((size_t)world_id);

    if (m_hot_reload_manager) {
        m_hot_reload_manager->attach_registry(w.state());
	}

    return w;
}

void scn::level::load_from_desc(const level_desc& desc, ecs::system_factory& sfactory, scn::ecs_assembler& assambler)
{
	m_current_level_tag = desc.get_tag();
	size_t world_index = 0;
    for (const auto& world_desc : desc.get_worlds()) {
        auto& world = create_world(world_desc->get_name(), world_index++);
        for (const auto& system_name : world_desc->get_systems()) {
            sfactory.create_system(system_name, world.state(), organizer());
		}

        entt::entity e = world.state().create();
		assambler.spawn_from_desc(world.state(), e, *world_desc, world_desc->get_name());
    }

    mark_systems_graphs_dirty();
}

void scn::level::load_world_from_desc(const world_desc& world_desc, ecs::system_factory& sfactory, scn::ecs_assembler& assambler)
{
    auto& world = create_world(world_desc.get_name(), 0);
    for (const auto& system_name : world_desc.get_systems()) {
        sfactory.create_system(system_name, world.state(), organizer());
    }

    entt::entity e = world.state().create();
    assambler.spawn_from_desc(world.state(), e, world_desc, world_desc.get_name());

    mark_systems_graphs_dirty();
}

void scn::level::reload_world(
	const std::string_view name,
	const world_desc& desc,
	ecs::system_factory& sf,
	scn::ecs_assembler& assambler)
{
	ASSERT_MSG(m_worlds.contains(std::string{name}), "reload_world: level doesn't contain world with this name");

	uint32_t old_id = m_worlds[std::string{name}]->get_id();

	if (m_hot_reload_manager) {
		m_hot_reload_manager->detach_registry(m_worlds[std::string{name}]->state());
	}

	m_worlds_by_id.erase(old_id);
	m_worlds.erase(std::string{name});

	clear_organizer();

	auto& w = create_world(name, old_id);

	for (const auto& sys_name : desc.get_systems()) {
		sf.create_system(sys_name, w.state(), organizer());
	}

	entt::entity e = w.state().create();
	assambler.spawn_from_desc(w.state(), e, desc, name);

	mark_systems_graphs_dirty();
}

void scn::level::load_systems_from_desc(const level_desc& desc, ecs::system_factory& system_factory)
{
    clear_organizer();
    for (const auto& world_desc : desc.get_worlds()) {
        for (const auto& system_name : world_desc->get_systems()) {
            auto& world = get_world(world_desc->get_name());
            system_factory.create_system(system_name, world.state(), organizer());
        }
    }
    mark_systems_graphs_dirty();
}

void scn::level::clear()
{
	for (const auto& [name, world] : m_worlds) {
        if (m_hot_reload_manager) {
            m_hot_reload_manager->detach_registry(world->state());
        }
    }

    m_level_state.clear();
    m_worlds.clear();
    m_worlds_by_id.clear();
    m_fixed_graph.clear();
    m_variable_graph.clear();
    m_level_state.ctx().emplace<ecs::runtime_context_provider>(
        ecs::runtime_context_provider::getter_type{
        [this](size_t world_id) { return this->create_runtime_context(world_id); }
        });
}

void scn::level::sync_point()
{
    for (auto& [name, world] : m_worlds) {
        auto& state = world->state();
        if (auto* changer = state.ctx().find<ecs::entity_changer>()) {
            changer->apply(state);
        }

        if (auto* sandbox = state.ctx().find<ecs::entity_spawner>()) {
            entt::continuous_loader loader{ state };
            {
                const auto& entity_storage = sandbox->storage<ecs::sandbox_entity>();
                ecs::sandbox_loader_archive archive{ loader, entity_storage };
                loader.get<entt::entity>(archive);
            }

            for (auto [id, storage] : sandbox->storage()) {
                if (id == entt::type_hash<ecs::sandbox_entity>::value()) continue;
                if (storage.empty()) continue;

                if (auto type = entt::resolve(id)) {
                    if (auto func = type.func(ecs::loader_get_h)) {
                        func.invoke({}, &loader, static_cast<const entt::basic_sparse_set<ecs::sandbox_entity>*>(&storage));
                    }
                }
            }

            sandbox->clear();
        }

    }

    if (m_hot_reload_manager) {
        m_hot_reload_manager->tick();
	}
}
