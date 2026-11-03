#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/scn_scene_layer.h>
#include <algorithm>
#include <utility>
#include <snk/common/eng_profiler.h>
#include <snk/common/logger/engine_log.h>

namespace {
	scn::context_set* g_current_set = nullptr;

	class current_set_scope
	{
	public:
		explicit current_set_scope(scn::context_set& set) : m_previous(std::exchange(g_current_set, &set)) {}
		~current_set_scope() { g_current_set = m_previous; }
		current_set_scope(const current_set_scope&) = delete;
		current_set_scope& operator=(const current_set_scope&) = delete;

	private:
		scn::context_set* m_previous;
	};
}

scn::context_set::context_set(std::string name, std::shared_ptr<scn::hot_reload_manager> hot_manager,
							  std::shared_ptr<ecs::channel_registry> channel_types, set_options options)
	: m_name(std::move(name))
	, m_options(options)
	, m_hot_reload_manager(options.hot_reload ? std::move(hot_manager) : std::shared_ptr<scn::hot_reload_manager>{})
	, m_channel(m_name, std::move(channel_types))
{
}

scn::context_set::~context_set()
{
	stop_tasks();
	clear();
}

void scn::context_set::stop_tasks()
{
	m_tasks.cancel_tasks();
	if (m_layer && m_layer->task_runner()) {
		m_layer->task_runner()->wait_idle(m_tasks);
	}
}

scn::context_set* scn::context_set::current()
{
	return g_current_set;
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
	const current_set_scope current_scope(*this);

	const ecs::run_mode mode = m_mode;
	begin_frame_channel(mode);

	for (const auto& [id, context] : m_contexts) {
		context->begin_frame(dt);
	}

	// A context runs all its phases back to back, contexts in id order. The set channel
	// exchanges only above, so this order cannot change what a system reads.
	for (const auto& [id, context] : m_contexts) {
		update_context(id, *context, mode);
	}

	if (mode == ecs::run_mode::step) {
		if (m_pending_steps > 0) --m_pending_steps;
		if (m_pending_steps == 0) m_mode = ecs::run_mode::pause;
	}
	sync_context_mode();
}

void scn::context_set::begin_frame_channel(ecs::run_mode mode)
{
	m_participants.clear();
	m_participants.reserve(m_contexts.size());
	for (const auto& [id, context] : m_contexts) {
		m_participants.push_back(ecs::channel_participant{ id, context->get_name(), &context->state() });
	}

	if (mode == ecs::run_mode::play || mode == ecs::run_mode::step) {
		m_channel.begin_frame(m_participants);
	} else {
		m_channel.skip_frame(m_participants);
	}
}

void scn::context_set::set_mode(ecs::run_mode mode)
{
	m_mode = mode;
	m_pending_steps = mode == ecs::run_mode::step ? 1 : 0;
	sync_context_mode();
}

void scn::context_set::step(uint32_t frames)
{
	if (!ENGINE_VERIFY(frames >= 1, "set '{}': step of 0 frames, running one", m_name)) {
		frames = 1;
	}
	set_mode(ecs::run_mode::step);
	m_pending_steps = frames;
}

void scn::context_set::set_time_scale(float scale)
{
	if (!ENGINE_VERIFY(scale >= 0.0f, "set '{}': time scale {} is negative or NaN, using 0", m_name, scale)) {
		scale = 0.0f;
	} else if (!ENGINE_VERIFY(scale <= max_time_scale, "set '{}': time scale {} is above {}, using {}", m_name, scale,
								max_time_scale, max_time_scale)) {
		scale = max_time_scale;
	}
	m_time_scale = scale;
	for (const auto& [id, context] : m_contexts) {
		context->set_time_scale(m_time_scale);
	}
}

void scn::context_set::sync_context_mode()
{
	for (const auto& [id, context] : m_contexts) {
		context->set_mode(m_mode);
	}
}

ecs::runtime_context scn::context_set::create_runtime_context(size_t own_id)
{
	return ecs::runtime_context{ .current_registry = get(static_cast<uint32_t>(own_id)).state(), .current_world_id = own_id };
}

void scn::context_set::update_context(uint32_t id, world_context& context, ecs::run_mode mode)
{
	for (size_t p = 0; p < m_phase_order.size(); ++p) {
		if (m_phase_order[p].builtin() == ecs::phase::fixed_update) {
			PROFILE_SCOPE("ECS.fixed_update");
			for (int i = 0; i < context.fixed_steps(); ++i) {
				context.write_fixed_time();
				run_context_phase(id, context, p, mode);
			}
		} else {
			run_context_phase(id, context, p, mode);
		}
	}
}

void scn::context_set::run_context_phase(uint32_t id, world_context& context, size_t order_index, ecs::run_mode mode)
{
	// Also a context without systems: commands queued into it from outside (other contexts'
	// systems, the editor) are applied on the same schedule, after each phase.
	if (context.has_phase_work(order_index)) {
		const ecs::runtime_context ctx = create_runtime_context(id);
		context.run_phase(order_index, mode, ctx);
	}
	context.apply_commands();
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
	if (m_layer) {
		w.set_meta(&m_layer->meta());
	}
	m_channel.attach(id, w.get_name(), w.state());
	w.seed(m_seed);
	w.set_mode(m_mode);
	w.set_time_scale(m_time_scale);
	// lockstep: a context created later joins at the same point of the step as the others
	if (lockstep_ref) w.set_stepper(*lockstep_ref);

	if (m_hot_reload_manager) {
		m_hot_reload_manager->attach_registry(w.state());
	}

	return w;
}

void scn::context_set::load_from_desc(const level_desc& desc, ecs::system_factory& sfactory, scn::ecs_assembler& assambler,
										 context_kind kind)
{
	const current_set_scope current_scope(*this);
	m_tag = desc.get_tag();
	// no leftover fraction of a step from the previous level
	for (const auto& [id, context] : m_contexts) {
		context->reset_time();
	}
	m_desc_world_names.clear();
	for (const auto& world_desc : desc.get_worlds()) {
		m_desc_world_names.emplace_back(world_desc->get_name());
		auto& world = create_context(world_desc->get_name(), acquire_free_id(), kind);
		world.set_input_layer(world_desc->get_input_layer());
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
	const current_set_scope current_scope(*this);
	ASSERT_MSG(m_context_ids.contains(std::string{name}), "reload_world: level doesn't contain world with this name");

	const uint32_t old_id = m_context_ids[std::string{name}];
	const context_kind kind = m_contexts.at(old_id)->kind();
	destroy_by_id(old_id);

	auto& w = create_context(name, old_id, kind);
	w.set_input_layer(desc.get_input_layer());
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
			context->set_input_layer(world_desc->get_input_layer());
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
	m_channel.clear();
}
