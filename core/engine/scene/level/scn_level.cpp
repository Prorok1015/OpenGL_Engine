#include "scn_level.h"
#include "scn_model.h"
#include "ecs_command_buffer.hpp"
#include "ecs_entity_changer.hpp"
#include "ecs_component.h"
#include <entt/fwd.hpp>
#include <algorithm>
#include <functional>
#include <queue>
#include "eng_profiler.h"

scn::level::level(std::shared_ptr<scn::hot_reload_manager> hot_manager)
	: m_hot_reload_manager(std::move(hot_manager))
{
    install_context_provider();
}

scn::level::~level()
{
    clear();
}

void scn::level::update(std::chrono::duration<float> dt)
{
    PROFILE_SCOPE("LevelUpdate");

    m_accumulator += dt.count();

    while (m_accumulator >= m_fixed_step) {
        PROFILE_SCOPE("ECS.FixedGraph");
        m_level_state.ctx().insert_or_assign<scn::fixed_time>({ m_fixed_step });

        run_graph(m_fixed_graph, m_fixed_order);

        m_accumulator -= m_fixed_step;
    }

    m_level_state.ctx().insert_or_assign<delta_time>({ dt.count() });

    float alpha = m_accumulator / m_fixed_step;
    m_level_state.ctx().insert_or_assign<scn::update_alpha>({ alpha });

    {
        PROFILE_SCOPE("ECS.VariableGraph");
        run_graph(m_variable_graph, m_variable_order);
    }
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
        add_world_systems(world, world_desc->get_systems(), sfactory);

        entt::entity e = world.state().create();
		assambler.spawn_from_desc(world.state(), e, *world_desc, world_desc->get_name());
    }

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

	auto& w = create_world(name, old_id);
	m_world_systems[std::string{ name }] = desc.get_systems();

	entt::entity e = w.state().create();
	assambler.spawn_from_desc(w.state(), e, desc, name);

	// The organizer still references the destroyed world's systems; rebuild it from every live world.
	rebuild_systems(sf);
}

void scn::level::remove_world(const std::string_view name, ecs::system_factory& sf)
{
	std::string key{name};
	ASSERT_MSG(m_worlds.contains(key), "remove_world: level doesn't contain world with this name");

	auto& world = m_worlds[key];
	uint32_t id = world->get_id();

	if (m_hot_reload_manager) {
		m_hot_reload_manager->detach_registry(world->state());
	}

	m_worlds_by_id.erase(id);
	m_worlds.erase(key);
	m_world_systems.erase(key);

	rebuild_systems(sf);
}

void scn::level::load_systems_from_desc(const level_desc& desc, ecs::system_factory& system_factory)
{
    for (const auto& world_desc : desc.get_worlds()) {
        m_world_systems[std::string{ world_desc->get_name() }] = world_desc->get_systems();
    }
    rebuild_systems(system_factory);
}

void scn::level::add_world_systems(
    scn::world& world,
    const std::vector<std::string>& systems,
    ecs::system_factory& sf)
{
    m_world_systems[std::string{ world.get_name() }] = systems;
    create_world_systems(world, sf);
    mark_systems_graphs_dirty();
}

void scn::level::create_world_systems(scn::world& world, ecs::system_factory& sf)
{
    const std::string world_name{ world.get_name() };
    auto it = m_world_systems.find(world_name);
    if (it == m_world_systems.end()) return;

    auto& handles = m_system_handles[world_name];
    for (const auto& system_name : it->second) {
        // Stateful systems are referenced by raw pointer from the organizer, keep them alive with it.
        if (auto sys = sf.create_system(system_name, world.state(), m_organizer)) {
            handles.push_back(std::move(sys));
        }
    }
}

void scn::level::rebuild_systems(ecs::system_factory& sf)
{
    clear_organizer();

    std::vector<scn::world*> ordered;
    ordered.reserve(m_worlds_by_id.size());
    for (const auto& [id, world] : m_worlds_by_id) {
        ordered.push_back(world);
    }
    std::ranges::sort(ordered, {}, &scn::world::get_id);

    for (scn::world* world : ordered) {
        create_world_systems(*world, sf);
    }
    mark_systems_graphs_dirty();
}

std::vector<size_t> scn::level::topological_order(const std::vector<entt::organizer::vertex>& graph)
{
    std::vector<size_t> in_degree(graph.size(), 0);
    for (const auto& node : graph) {
        for (size_t out : node.out_edges()) {
            ++in_degree[out];
        }
    }

    std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready;
    for (size_t i = 0; i < graph.size(); ++i) {
        if (in_degree[i] == 0) ready.push(i);
    }

    std::vector<size_t> order;
    order.reserve(graph.size());
    while (!ready.empty()) {
        const size_t idx = ready.top();
        ready.pop();
        order.push_back(idx);
        for (size_t out : graph[idx].out_edges()) {
            if (--in_degree[out] == 0) ready.push(out);
        }
    }

    ASSERT_MSG(order.size() == graph.size(), "system graph contains a cycle");
    return order;
}

void scn::level::clear()
{
	for (const auto& [name, world] : m_worlds) {
        if (m_hot_reload_manager) {
            m_hot_reload_manager->detach_registry(world->state());
        }
    }

    clear_organizer();
    m_world_systems.clear();
    m_level_state.clear();
    m_worlds.clear();
    m_worlds_by_id.clear();
    install_context_provider();
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
