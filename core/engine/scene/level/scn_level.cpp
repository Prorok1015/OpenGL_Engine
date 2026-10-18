#include "scn_level.h"
#include "scn_model.h"
#include "ecs_command_buffer.hpp"
#include "ecs_entity_changer.hpp"
#include "ecs_component.h"
#include <entt/fwd.hpp>
#include <algorithm>
#include <functional>
#include "eng_profiler.h"

scn::level::level(std::shared_ptr<scn::hot_reload_manager> hot_manager)
	: m_hot_reload_manager(std::move(hot_manager))
{
    m_level_state.ctx().insert_or_assign(ecs::rng{ ecs::derive_seed(m_seed, LEVEL_ID) });
}

void scn::level::set_seed(uint64_t seed)
{
    m_seed = seed;
    m_level_state.ctx().insert_or_assign(ecs::rng{ ecs::derive_seed(m_seed, LEVEL_ID) });
    for (const auto& [id, world] : m_worlds_by_id) {
        world->state().ctx().insert_or_assign(ecs::rng{ ecs::derive_seed(m_seed, id) });
    }
}

scn::level::~level()
{
    clear();
}

void scn::level::update(std::chrono::duration<float> dt)
{
    PROFILE_SCOPE("LevelUpdate");

    const ecs::run_mode mode = m_mode;
    float frame_dt = 0.0f;
    int fixed_steps = 0;
    switch (mode) {
    case ecs::run_mode::pause:
        break; // time is frozen
    case ecs::run_mode::step:
        frame_dt = m_stepper.step();
        fixed_steps = 1;
        break;
    case ecs::run_mode::edit:
    case ecs::run_mode::play:
        frame_dt = static_cast<float>(m_stepper.clamp_dt(dt.count()));
        fixed_steps = m_stepper.advance(frame_dt);
        break;
    }

    m_level_state.ctx().insert_or_assign<delta_time>({ frame_dt });
    m_level_state.ctx().insert_or_assign<scn::update_alpha>({ m_stepper.alpha() });

    size_t phase_count = 0;
    for (const auto& [id, plan] : m_plans) {
        phase_count = std::max(phase_count, plan.phases.size());
    }

    for (size_t p = 0; p < phase_count; ++p) {
        const auto* any = [&]() -> const ecs::phase_plan* {
            for (const auto& [id, plan] : m_plans) {
                if (p < plan.phases.size()) return &plan.phases[p];
            }
            return nullptr;
        }();
        const bool fixed = any && any->id.builtin() == ecs::phase::fixed_update;
        if (fixed) {
            PROFILE_SCOPE("ECS.fixed_update");
            for (int i = 0; i < fixed_steps; ++i) {
                m_level_state.ctx().insert_or_assign<scn::fixed_time>({ m_stepper.step() });
                run_phase(p, mode);
            }
        } else {
            run_phase(p, mode);
        }
    }

    if (mode == ecs::run_mode::step) m_mode = ecs::run_mode::pause;

    if (m_hot_reload_manager) {
        m_hot_reload_manager->tick();
	}
}

void scn::level::run_phase(size_t phase_index, ecs::run_mode mode)
{
    for (auto& [world_id, plan] : m_plans) {
        if (phase_index >= plan.phases.size()) continue;
        const auto& phase = plan.phases[phase_index];
        if (!phase.nodes.empty()) {
            PROFILE_SCOPE(phase.id.name().c_str());
            const ecs::runtime_context ctx = create_runtime_context(world_id);
            for (const auto& node : phase.nodes) {
                if (!ecs::mode_runs_system(mode, node.run_in_edit)) continue;
                PROFILE_SCOPE(node.name.c_str());
                node(ctx);
            }
        }
        apply_commands(get_world(world_id));
    }
}

scn::world& scn::level::create_world(const std::string_view type, uint32_t world_id)
{
    auto& w = *(m_worlds[std::string{ type }] = std::make_unique<scn::world>(world_id, type));
    m_worlds_by_id[world_id] = &w; // Register in ID map
    w.state().ctx().insert_or_assign(ecs::rng{ ecs::derive_seed(m_seed, world_id) });

    if (m_hot_reload_manager) {
        m_hot_reload_manager->attach_registry(w.state());
	}

    return w;
}

void scn::level::load_from_desc(const level_desc& desc, ecs::system_factory& sfactory, scn::ecs_assembler& assambler)
{
	m_current_level_tag = desc.get_tag();
	m_level_state.ctx().insert_or_assign(ecs::rng{ ecs::derive_seed(m_seed, LEVEL_ID) }); // a fresh load replays from the seed
	size_t world_index = 0;
    for (const auto& world_desc : desc.get_worlds()) {
        auto& world = create_world(world_desc->get_name(), world_index++);
        add_world_systems(world, world_desc->get_systems(), sfactory);

        entt::entity e = world.state().create();
		assambler.spawn_from_desc(world.state(), e, *world_desc, world_desc->get_name());
    }
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

	m_plans.erase(old_id);
	m_worlds_by_id.erase(old_id);
	m_worlds.erase(std::string{name});

	auto& w = create_world(name, old_id);
	m_world_systems[std::string{ name }] = desc.get_systems();
	rebuild_world_plan(old_id, sf);

	entt::entity e = w.state().create();
	assambler.spawn_from_desc(w.state(), e, desc, name);
}

void scn::level::remove_world(const std::string_view name)
{
	std::string key{name};
	ASSERT_MSG(m_worlds.contains(key), "remove_world: level doesn't contain world with this name");

	auto& world = m_worlds[key];
	uint32_t id = world->get_id();

	if (m_hot_reload_manager) {
		m_hot_reload_manager->detach_registry(world->state());
	}

	m_plans.erase(id);
	m_worlds_by_id.erase(id);
	m_worlds.erase(key);
	m_world_systems.erase(key);
}

void scn::level::load_systems_from_desc(const level_desc& desc, ecs::system_factory& system_factory)
{
    for (const auto& world_desc : desc.get_worlds()) {
        m_world_systems[std::string{ world_desc->get_name() }] = world_desc->get_systems();
    }
    rebuild_all_plans(system_factory);
}

void scn::level::add_world_systems(
    scn::world& world,
    const std::vector<std::string>& systems,
    ecs::system_factory& sf)
{
    m_world_systems[std::string{ world.get_name() }] = systems;
    rebuild_world_plan(world.get_id(), sf);
}

void scn::level::rebuild_world_plan(uint32_t world_id, ecs::system_factory& sf)
{
    const scn::world& world = get_world(world_id);
    auto it = m_world_systems.find(std::string{ world.get_name() });
    if (it == m_world_systems.end()) {
        m_plans.erase(world_id);
        return;
    }
    m_plans.insert_or_assign(world_id, ecs::build_system_plan(sf.registry(), it->second));
}

void scn::level::rebuild_all_plans(ecs::system_factory& sf)
{
    m_plans.clear();
    for (const auto& [id, world] : m_worlds_by_id) {
        rebuild_world_plan(id, sf);
    }
}

void scn::level::clear()
{
	for (const auto& [name, world] : m_worlds) {
        if (m_hot_reload_manager) {
            m_hot_reload_manager->detach_registry(world->state());
        }
    }

    m_plans.clear();
    m_world_systems.clear();
    m_level_state.clear();
    m_worlds.clear();
    m_worlds_by_id.clear();
}

void scn::level::apply_commands(scn::world& world)
{
    auto& state = world.state();
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
