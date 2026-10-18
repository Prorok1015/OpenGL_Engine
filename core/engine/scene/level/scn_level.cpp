#include <snk/scene/level/scn_level.h>
#include <snk/scene/scn_model.h>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <snk/ecs/ecs_component.h>
#include <entt/fwd.hpp>
#include <algorithm>
#include <functional>
#include <snk/common/eng_profiler.h>
#include <snk/common/logger/engine_log.h>

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

    for (size_t p = 0; p < m_phase_order.size(); ++p) {
        const bool fixed = m_phase_order[p].builtin() == ecs::phase::fixed_update;
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

void scn::level::run_phase(size_t order_index, ecs::run_mode mode)
{
    // Every world takes part, also one without systems: commands queued into it from outside
    // (other worlds' systems, the editor) are applied on the same schedule.
    for (const auto& [world_id, world] : m_worlds_by_id) {
        const ecs::phase_plan* phase = nullptr;
        if (auto slots = m_plan_slots.find(world_id); slots != m_plan_slots.end()) {
            phase = slots->second[order_index];
        }
        if (phase && !phase->nodes.empty()) {
            PROFILE_SCOPE(phase->id.name().c_str());
            const ecs::runtime_context ctx = create_runtime_context(world_id);
            for (const auto& node : phase->nodes) {
                if (!ecs::mode_runs_system(mode, node.run_in_edit)) continue;
                PROFILE_SCOPE(node.name.c_str());
                node(ctx);
            }
        }
        apply_commands(world->state());
    }
    // Systems reach the level state through get_world_registry(LEVEL_ID) as well.
    apply_commands(m_level_state);
}

scn::world& scn::level::create_world(const std::string_view type, uint32_t world_id)
{
    if (auto it = m_worlds.find(std::string{ type }); it != m_worlds.end()) {
        // replacing would free a world that m_worlds_by_id and m_plans still point at
        egLOG_ERROR("scene", "create_world: world '{}' already exists, keeping the existing one", type);
        return *it->second;
    }

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
	m_stepper.reset(); // no leftover fraction of a step from the previous level
	m_level_state.ctx().insert_or_assign<scn::update_alpha>({ 0.0f });
    for (const auto& world_desc : desc.get_worlds()) {
        auto& world = create_world(world_desc->get_name(), acquire_free_world_id());
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
	m_plan_slots.erase(old_id);
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
	m_plan_slots.erase(id);
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
    } else {
        m_plans.insert_or_assign(world_id, ecs::build_system_plan(sf.registry(), it->second));
    }
    refresh_plan_slots(sf);
}

void scn::level::rebuild_all_plans(ecs::system_factory& sf)
{
    m_plans.clear();
    for (const auto& [id, world] : m_worlds_by_id) {
        rebuild_world_plan(id, sf);
    }
    refresh_plan_slots(sf); // also when there are no worlds: the phase order still follows the table
}

void scn::level::refresh_plan_slots(const ecs::system_factory& sf)
{
    m_phase_order = sf.registry().phases().ordered();
    m_plan_slots.clear();
    for (const auto& [id, plan] : m_plans) {
        auto& slots = m_plan_slots[id];
        slots.reserve(m_phase_order.size());
        for (const auto& pid : m_phase_order) {
            slots.push_back(plan.find_phase(pid));
        }
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
    m_plan_slots.clear();
    m_stepper.reset();
    m_world_systems.clear();
    m_level_state.clear();
    m_worlds.clear();
    m_worlds_by_id.clear();
}

void scn::level::apply_commands(entt::registry& state)
{
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
