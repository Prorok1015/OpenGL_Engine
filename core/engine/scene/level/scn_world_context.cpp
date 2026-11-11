#include <snk/scene/level/scn_world_context.h>
#include "scn_prefab_spawning.h"
#include <snk/input/inp_action_state.h>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_random.hpp>
#include <snk/common/eng_profiler.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_schema_components.h>
#include <entt/entity/snapshot.hpp>
#include <cstdint>
#include <optional>
#include <unordered_set>

namespace
{
	using sandbox_blob_pool = entt::storage_for_t<desc::dyn_blob, ecs::sandbox_entity>;

	// The world entities the loader made for the sandbox ones. A raw id the sandbox does not have may name another
	// entity of the world: it reads as none, as an entity member of a C++ component does.
	class loader_remap final : public desc::entity_remap
	{
	public:
		explicit loader_remap(const entt::continuous_loader& loader) : m_loader(loader) {}

		std::optional<std::uint32_t> find(std::uint32_t saved) const override
		{
			const auto entity = static_cast<entt::entity>(saved);
			if (!m_loader.contains(entity)) {
				return std::nullopt;
			}
			return entt::to_integral(m_loader.map(entity));
		}

	private:
		const entt::continuous_loader& m_loader;
	};

	// The sandbox pool of a component without a C++ type, into the world entities the loader made.
	void move_schema_pool(entt::registry& state, const entt::continuous_loader& loader, const sandbox_blob_pool& pool, entt::id_type id,
		const desc::schema_binding& schema)
	{
		const loader_remap entities{ loader };
		for (const ecs::sandbox_entity owner : static_cast<const entt::basic_sparse_set<ecs::sandbox_entity>&>(pool)) {
			const auto target = static_cast<entt::entity>(entt::to_integral(owner));
			if (!loader.contains(target)) {
				continue;
			}
			desc::dyn_blob blob = pool.get(owner);
			// Made by module code against another layout of the schema: reading it by this one's offsets overruns it.
			if (blob.bytes.size() != schema.schema->layout.size) {
				egLOG_ERROR("ecs", "a '{}' component from the commands is dropped: its blob is not laid out by the registered schema", schema.schema->name);
				continue;
			}
			desc::dyn_ref ref{ &blob, 0 };
			desc::remap_entities(schema, &ref, entities);
			scn::emplace_schema_component(state, loader.map(target), id, std::move(blob));
		}
	}
}

scn::world_context::world_context(uint32_t id, std::string_view name)
	: m_id(id), m_name(name)
{
	// Present from the start, so a system never sees a missing time resource.
	m_state.ctx().insert_or_assign(scn::delta_time{ 0.0f });
	m_state.ctx().insert_or_assign(scn::update_alpha{ 0.0f });
	m_state.ctx().insert_or_assign(scn::world_time{ 0.0 });
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
		// Scaled before the clamp: max_frame_dt keeps bounding the simulated time of one frame.
		frame_dt = static_cast<float>(m_stepper.clamp_dt(static_cast<double>(dt.count()) * m_time_scale));
		m_fixed_steps = m_stepper.advance(frame_dt);
		break;
	}
	m_state.ctx().insert_or_assign(scn::delta_time{ frame_dt });
	m_state.ctx().insert_or_assign(scn::update_alpha{ m_stepper.alpha() });
	m_state.ctx().get<scn::world_time>().seconds += frame_dt;
}

void scn::world_context::write_fixed_time()
{
	m_state.ctx().insert_or_assign(scn::fixed_time{ m_stepper.step() });
	// The one place every fixed step starts, also before the first system of the phase: no step can miss its edges.
	if (auto* actions = m_state.ctx().find<inp::fixed_action_state>()) actions->begin_step();
}

void scn::world_context::reset_time()
{
	m_stepper.reset();
	m_state.ctx().insert_or_assign(scn::update_alpha{ 0.0f });
	m_state.ctx().insert_or_assign(scn::world_time{ 0.0 });
}

void scn::world_context::restore_time(double world_time, double fixed_accumulator)
{
	m_stepper.set_accumulator(fixed_accumulator);
	m_state.ctx().insert_or_assign(scn::update_alpha{ m_stepper.alpha() });
	m_state.ctx().insert_or_assign(scn::world_time{ world_time });
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

void scn::world_context::apply_commands(entt::registry& state, const entt::meta_ctx* layer_meta)
{
	apply_commands(state, layer_meta, nullptr);
}

void scn::world_context::apply_commands(entt::registry& state, const entt::meta_ctx* layer_meta, const component_catalog* catalog)
{
	if (auto* changer = state.ctx().find<ecs::entity_changer>()) {
		changer->apply(state);
	}
	// After the changer: its batches may still name an entity of a tree removed here.
	scn::detail::flush_despawns(state);

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

			if (auto type = ecs::find_meta(layer_meta, id)) {
				if (auto func = type.func(ecs::loader_get_h)) {
					func.invoke({}, &loader, static_cast<const entt::basic_sparse_set<ecs::sandbox_entity>*>(&storage));
					continue;
				}
			}
			if (catalog && storage.type() == entt::type_id<desc::dyn_blob>()) {
				if (const scn::component_entry* entry = catalog->find(id); entry && scn::is_schema_component(*entry)) {
					move_schema_pool(state, loader, static_cast<const sandbox_blob_pool&>(storage), id, *entry->schema);
					continue;
				}
			}
			// Not silent (#606): a component without meta, or one without a C++ type the catalog does not know, loses
			// what the commands gave it. Once per storage id, the drop repeats every frame. Unsynchronized:
			// apply_commands runs on the main thread only.
			static std::unordered_set<entt::id_type> warned;
			if (warned.insert(id).second) {
				egLOG_WARN("ecs", "commands for the component of storage id {} are dropped: it has no meta to move it into the world", id);
			}
		}

		sandbox->clear();
	}
}
