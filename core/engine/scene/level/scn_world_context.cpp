#include <snk/scene/level/scn_world_context.h>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_random.hpp>
#include <snk/common/eng_profiler.h>

scn::world_context::world_context(uint32_t id, std::string_view name)
	: m_id(id), m_name(name)
{
	// Present from the start, so a system never sees a missing time resource.
	m_state.ctx().insert_or_assign(scn::delta_time{ 0.0f });
	m_state.ctx().insert_or_assign(scn::update_alpha{ 0.0f });
	m_state.ctx().insert_or_assign(scn::fixed_time{ m_stepper.step() });
}

void scn::world_context::seed(uint64_t base_seed)
{
	m_state.ctx().insert_or_assign(ecs::rng{ ecs::derive_seed(base_seed, m_id) });
}

void scn::world_context::begin_frame(std::chrono::duration<float> dt)
{
	float frame_dt = 0.0f;
	m_fixed_steps = 0;
	switch (m_mode) {
	case ecs::run_mode::pause:
		break; // time is frozen
	case ecs::run_mode::step:
		frame_dt = m_stepper.step();
		m_fixed_steps = 1;
		break;
	case ecs::run_mode::edit:
	case ecs::run_mode::play:
		frame_dt = static_cast<float>(m_stepper.clamp_dt(dt.count()));
		m_fixed_steps = m_stepper.advance(frame_dt);
		break;
	}
	m_state.ctx().insert_or_assign(scn::delta_time{ frame_dt });
	m_state.ctx().insert_or_assign(scn::update_alpha{ m_stepper.alpha() });
}

void scn::world_context::write_fixed_time()
{
	m_state.ctx().insert_or_assign(scn::fixed_time{ m_stepper.step() });
}

void scn::world_context::reset_time()
{
	m_stepper.reset();
	m_state.ctx().insert_or_assign(scn::update_alpha{ 0.0f });
}

void scn::world_context::set_stepper(const ecs::fixed_stepper& stepper)
{
	m_stepper = stepper;
	m_state.ctx().insert_or_assign(scn::update_alpha{ m_stepper.alpha() });
}

void scn::world_context::rebuild_plan(const ecs::system_registry& registry)
{
	if (m_systems) {
		m_plan = ecs::build_system_plan(registry, *m_systems);
	} else {
		m_plan.reset();
		m_slots.clear();
	}
}

void scn::world_context::resolve_slots(const std::vector<ecs::phase_id>& phase_order)
{
	m_slots.clear();
	if (!m_plan) return;
	m_slots.reserve(phase_order.size());
	for (const auto& pid : phase_order) {
		m_slots.push_back(m_plan->find_phase(pid));
	}
}

bool scn::world_context::has_phase_work(size_t order_index) const
{
	return order_index < m_slots.size() && m_slots[order_index] && !m_slots[order_index]->nodes.empty();
}

void scn::world_context::run_phase(size_t order_index, ecs::run_mode mode, const ecs::runtime_context& ctx) const
{
	if (!has_phase_work(order_index)) return;
	const ecs::phase_plan& phase = *m_slots[order_index];
	PROFILE_SCOPE(phase.id.name().c_str());
	for (const auto& node : phase.nodes) {
		if (!ecs::mode_runs_system(mode, node.run_in_edit)) continue;
		PROFILE_SCOPE(node.name.c_str());
		node(ctx);
	}
}

void scn::world_context::apply_commands(entt::registry& state)
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
