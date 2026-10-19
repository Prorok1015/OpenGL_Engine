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
}

void scn::level::set_seed(uint64_t seed)
{
    m_seed = seed;
    for (const auto& [id, context] : m_contexts) {
        context->seed(m_seed);
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
    int fixed_steps = 0;
    for (const auto& [id, context] : m_contexts) {
        context->begin_frame(dt);
        fixed_steps = std::max(fixed_steps, context->fixed_steps());
    }

    for (size_t p = 0; p < m_phase_order.size(); ++p) {
        const bool fixed = m_phase_order[p].builtin() == ecs::phase::fixed_update;
        if (fixed) {
            PROFILE_SCOPE("ECS.fixed_update");
            for (int i = 0; i < fixed_steps; ++i) {
                run_phase(p, mode, i);
            }
        } else {
            run_phase(p, mode);
        }
    }

    if (mode == ecs::run_mode::step) m_mode = ecs::run_mode::pause;
    sync_context_mode();

    if (m_hot_reload_manager) {
        m_hot_reload_manager->tick();
	}
}

void scn::level::set_mode(ecs::run_mode mode)
{
    m_mode = mode;
    sync_context_mode();
}

void scn::level::sync_context_mode()
{
    for (const auto& [id, context] : m_contexts) {
        context->set_mode(m_mode);
    }
}

void scn::level::run_phase(size_t order_index, ecs::run_mode mode, int fixed_step)
{
    // Every context takes part, also one without systems: commands queued into it from outside
    // (other worlds' systems, the editor) are applied on the same schedule.
    for (const auto& [world_id, context] : m_contexts) {
        if (fixed_step >= 0) {
            if (fixed_step >= context->fixed_steps()) continue;
            context->write_fixed_time();
        }
        if (context->has_phase_work(order_index)) {
            const ecs::runtime_context ctx = create_runtime_context(world_id);
            context->run_phase(order_index, mode, ctx);
        }
        context->apply_commands();
    }
}

scn::world& scn::level::create_world(const std::string_view type, uint32_t world_id)
{
    if (auto it = m_context_ids.find(std::string{ type }); it != m_context_ids.end()) {
        // replacing would free a context that the id map still points at
        egLOG_ERROR("scene", "create_world: world '{}' already exists, keeping the existing one", type);
        return *m_contexts.at(it->second);
    }
    ASSERT_MSG(!m_contexts.contains(world_id), "create_world: world id is already in use");

    const ecs::fixed_stepper* lockstep_ref = m_contexts.empty() ? nullptr : &m_contexts.begin()->second->stepper();
    auto& w = *(m_contexts[world_id] = std::make_unique<scn::world_context>(world_id, type));
    m_context_ids[std::string{ type }] = world_id;
    w.seed(m_seed);
    w.set_mode(m_mode);
    // lockstep: a context created later joins at the same point of the step as the others
    if (lockstep_ref) w.set_stepper(*lockstep_ref);

    if (m_hot_reload_manager) {
        m_hot_reload_manager->attach_registry(w.state());
	}

    return w;
}

void scn::level::load_from_desc(const level_desc& desc, ecs::system_factory& sfactory, scn::ecs_assembler& assambler)
{
	m_current_level_tag = desc.get_tag();
	// no leftover fraction of a step from the previous level
	for (const auto& [id, context] : m_contexts) {
		context->reset_time();
	}
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
	ASSERT_MSG(m_context_ids.contains(std::string{name}), "reload_world: level doesn't contain world with this name");

	const uint32_t old_id = m_context_ids[std::string{name}];
	destroy_context(old_id);

	auto& w = create_world(name, old_id);
	add_world_systems(w, desc.get_systems(), sf);

	entt::entity e = w.state().create();
	assambler.spawn_from_desc(w.state(), e, desc, name);
}

void scn::level::remove_world(const std::string_view name)
{
	ASSERT_MSG(m_context_ids.contains(std::string{name}), "remove_world: level doesn't contain world with this name");
	destroy_context(m_context_ids[std::string{name}]);
}

void scn::level::destroy_context(uint32_t id)
{
	auto it = m_contexts.find(id);
	if (m_hot_reload_manager) {
		m_hot_reload_manager->detach_registry(it->second->state());
	}
	m_context_ids.erase(std::string{ it->second->get_name() });
	m_contexts.erase(it);
}

void scn::level::load_systems_from_desc(const level_desc& desc, ecs::system_factory& system_factory)
{
    for (const auto& world_desc : desc.get_worlds()) {
        if (auto* context = find_world(world_desc->get_name())) {
            context->set_systems(world_desc->get_systems());
        }
    }
    rebuild_all_plans(system_factory);
}

void scn::level::add_world_systems(
    scn::world& world,
    const std::vector<std::string>& systems,
    ecs::system_factory& sf)
{
    world.set_systems(systems);
    rebuild_world_plan(world.get_id(), sf);
}

void scn::level::rebuild_world_plan(uint32_t world_id, ecs::system_factory& sf)
{
    get_world(world_id).rebuild_plan(sf.registry());
    refresh_plan_slots(sf);
}

void scn::level::rebuild_all_plans(ecs::system_factory& sf)
{
    for (const auto& [id, context] : m_contexts) {
        context->rebuild_plan(sf.registry());
    }
    refresh_plan_slots(sf); // also when there are no worlds: the phase order still follows the table
}

void scn::level::refresh_plan_slots(const ecs::system_factory& sf)
{
    m_phase_order = sf.registry().phases().ordered();
    for (const auto& [id, context] : m_contexts) {
        context->resolve_slots(m_phase_order);
    }
}

void scn::level::clear()
{
	for (const auto& [id, context] : m_contexts) {
        if (m_hot_reload_manager) {
            m_hot_reload_manager->detach_registry(context->state());
        }
    }

    m_contexts.clear();
    m_context_ids.clear();
}
