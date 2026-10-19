#include <snk/scene/level/scn_context_set.h>
#include <algorithm>
#include <snk/common/eng_profiler.h>
#include <snk/common/logger/engine_log.h>

scn::context_set::context_set(std::string name, std::shared_ptr<scn::hot_reload_manager> hot_manager)
	: m_name(std::move(name))
	, m_hot_reload_manager(std::move(hot_manager))
{
}

scn::context_set::~context_set()
{
	clear();
}

void scn::context_set::set_seed(uint64_t seed)
{
	m_seed = seed;
	for (const auto& [id, context] : m_contexts) {
		context->seed(m_seed);
	}
}

void scn::context_set::update(std::chrono::duration<float> dt)
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
}

void scn::context_set::set_mode(ecs::run_mode mode)
{
	m_mode = mode;
	sync_context_mode();
}

void scn::context_set::sync_context_mode()
{
	for (const auto& [id, context] : m_contexts) {
		context->set_mode(m_mode);
	}
}

ecs::runtime_context scn::context_set::create_runtime_context(size_t own_id)
{
	ecs::runtime_context ctx{ .current_registry = get(static_cast<uint32_t>(own_id)).state(), .current_world_id = own_id };
	ctx.get_world_registry = [this, own_id](size_t world_id) -> entt::registry& {
		if (world_id == LEVEL_ID) return get(static_cast<uint32_t>(own_id)).state();
		return get(static_cast<uint32_t>(world_id)).state();
	};
	return ctx;
}

void scn::context_set::run_phase(size_t order_index, ecs::run_mode mode, int fixed_step)
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

scn::world_context& scn::context_set::create_context(std::string_view name, uint32_t id, context_kind kind)
{
	if (auto it = m_context_ids.find(std::string{ name }); it != m_context_ids.end()) {
		// replacing would free a context that the id map still points at
		egLOG_ERROR("scene", "create_world: world '{}' already exists, keeping the existing one", name);
		return *m_contexts.at(it->second);
	}
	ASSERT_MSG(!m_contexts.contains(id), "create_world: world id is already in use");

	const ecs::fixed_stepper* lockstep_ref = m_contexts.empty() ? nullptr : &m_contexts.begin()->second->stepper();
	auto& w = *(m_contexts[id] = std::make_unique<scn::world_context>(id, name));
	m_context_ids[std::string{ name }] = id;
	w.set_kind(kind);
	w.seed(m_seed);
	w.set_mode(m_mode);
	// lockstep: a context created later joins at the same point of the step as the others
	if (lockstep_ref) w.set_stepper(*lockstep_ref);

	if (m_hot_reload_manager) {
		m_hot_reload_manager->attach_registry(w.state());
	}

	return w;
}

void scn::context_set::load_from_desc(const level_desc& desc, ecs::system_factory& sfactory, scn::ecs_assembler& assambler)
{
	m_tag = desc.get_tag();
	// no leftover fraction of a step from the previous level
	for (const auto& [id, context] : m_contexts) {
		context->reset_time();
	}
	for (const auto& world_desc : desc.get_worlds()) {
		auto& world = create_context(world_desc->get_name(), acquire_free_id(), context_kind::level);
		add_systems(world, world_desc->get_systems(), sfactory);

		entt::entity e = world.state().create();
		assambler.spawn_from_desc(world.state(), e, *world_desc, world_desc->get_name());
	}
}

void scn::context_set::reload_context(
	std::string_view name,
	const world_desc& desc,
	ecs::system_factory& sf,
	scn::ecs_assembler& assambler)
{
	ASSERT_MSG(m_context_ids.contains(std::string{name}), "reload_world: level doesn't contain world with this name");

	const uint32_t old_id = m_context_ids[std::string{name}];
	const context_kind kind = m_contexts.at(old_id)->kind();
	destroy_by_id(old_id);

	auto& w = create_context(name, old_id, kind);
	add_systems(w, desc.get_systems(), sf);

	entt::entity e = w.state().create();
	assambler.spawn_from_desc(w.state(), e, desc, name);
}

void scn::context_set::destroy_context(std::string_view name)
{
	ASSERT_MSG(m_context_ids.contains(std::string{name}), "remove_world: level doesn't contain world with this name");
	destroy_by_id(m_context_ids[std::string{name}]);
}

void scn::context_set::destroy_by_id(uint32_t id)
{
	auto it = m_contexts.find(id);
	if (m_hot_reload_manager) {
		m_hot_reload_manager->detach_registry(it->second->state());
	}
	m_context_ids.erase(std::string{ it->second->get_name() });
	m_contexts.erase(it);
}

void scn::context_set::load_systems_from_desc(const level_desc& desc, ecs::system_factory& system_factory)
{
	for (const auto& world_desc : desc.get_worlds()) {
		if (auto* context = find(world_desc->get_name())) {
			context->set_systems(world_desc->get_systems());
		}
	}
	rebuild_all_plans(system_factory);
}

void scn::context_set::add_systems(
	scn::world_context& context,
	const std::vector<std::string>& systems,
	ecs::system_factory& sf)
{
	context.set_systems(systems);
	rebuild_context_plan(context.get_id(), sf);
}

void scn::context_set::rebuild_context_plan(uint32_t id, ecs::system_factory& sf)
{
	get(id).rebuild_plan(sf.registry());
	refresh_plan_slots(sf);
}

void scn::context_set::rebuild_all_plans(ecs::system_factory& sf)
{
	for (const auto& [id, context] : m_contexts) {
		context->rebuild_plan(sf.registry());
	}
	refresh_plan_slots(sf); // also when there are no contexts: the phase order still follows the table
}

void scn::context_set::refresh_plan_slots(const ecs::system_factory& sf)
{
	m_phase_order = sf.registry().phases().ordered();
	for (const auto& [id, context] : m_contexts) {
		context->resolve_slots(m_phase_order);
	}
}

void scn::context_set::clear()
{
	for (const auto& [id, context] : m_contexts) {
		if (m_hot_reload_manager) {
			m_hot_reload_manager->detach_registry(context->state());
		}
	}

	m_contexts.clear();
	m_context_ids.clear();
}
