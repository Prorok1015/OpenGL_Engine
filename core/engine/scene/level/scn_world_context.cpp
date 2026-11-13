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
#include <entt/meta/meta.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace
{
	using sandbox_blob_pool = entt::storage_for_t<desc::dyn_blob, ecs::sandbox_entity>;

	// What entt::basic_continuous_loader::get reads, served from a sandbox storage. Every archive(entity) is the next
	// packed slot, from 0 up: the loader asks for an entity once per element, then for its value only when the type
	// has one, so the entity read is what advances (#708). The entity storage's live ids are its first free_list()
	// slots; the rest are ids the sandbox released, which the loader reads after `in_use` and must not restore. Only
	// the engine reads through it (the entity pool and dyn_blob pools); a module's C++ component goes through
	// load_component_pool (#784).
	template<typename Storage>
	struct sandbox_loader_archive {
		static constexpr bool is_entity_storage = std::is_same_v<typename Storage::element_type, ecs::sandbox_entity>;
		// The loader's counts are entity_type: were it std::size_t, `in_use` would reach operator()(std::size_t&) and
		// every released id would be restored as live.
		static_assert(!std::is_same_v<typename entt::entt_traits<entt::entity>::entity_type, std::size_t>,
			"sandbox_loader_archive tells the element count from the in-use count by the entity_type overload");

		entt::continuous_loader& loader;
		const Storage& storage;
		std::size_t next = 0;
		ecs::sandbox_entity current = entt::null;
		int counts_read = 0;

		sandbox_loader_archive(entt::continuous_loader& l, const Storage& s)
			: loader(l), storage(s) {
		}

		void operator()(std::size_t& len) {
			len = storage.size();
		}

		// The element count, then, for the entity storage only, how many of them are live.
		template<typename Type>
			requires (std::is_same_v<Type, typename entt::entt_traits<entt::entity>::entity_type> && !std::is_same_v<Type, std::size_t>)
		void operator()(Type& count) {
			if constexpr (is_entity_storage) {
				count = static_cast<Type>(counts_read++ == 0 ? storage.size() : storage.free_list());
			} else {
				count = static_cast<Type>(storage.size());
			}
		}

		void operator()(entt::entity& ent) {
			if (next < storage.size()) {
				current = storage.data()[next++];
				ent = ecs::to_main(current);
			} else {
				current = entt::null;
				ent = entt::null;
			}
		}

		template<typename T>
		void operator()(T& out_component) {
			if constexpr (std::is_same_v<std::remove_cvref_t<T>, entt::entity>) {
				out_component = entt::null;
			} else if (current != entt::null) {
				out_component = storage.get(current);
			}
		}
	};

	bool has_entity_fields(const entt::meta_type& type)
	{
		for (auto&& [id, data] : type.data()) {
			const entt::type_info field = data.type().info();
			if (field == entt::type_id<entt::entity>() || field == entt::type_id<ecs::sandbox_entity>()) {
				return true;
			}
		}
		return false;
	}

	// Writes the world ids of the sandbox ids into the entity fields of `handle`, by the meta of the context the
	// component is registered in (the layer's or the engine's, #738), not by the default one.
	void remap_entity_fields(const entt::meta_type& type, entt::meta_any& handle, const entt::continuous_loader& loader)
	{
		for (auto&& [id, data] : type.data()) {
			const entt::type_info field = data.type().info();
			if (field == entt::type_id<entt::entity>()) {
				const entt::meta_any value = data.get(handle);
				if (const auto* raw = value.try_cast<entt::entity>()) {
					data.set(handle, loader.map(*raw));
				}
			} else if (field == entt::type_id<ecs::sandbox_entity>()) {
				const entt::meta_any value = data.get(handle);
				if (const auto* raw = value.try_cast<ecs::sandbox_entity>()) {
					data.set(handle, static_cast<ecs::sandbox_entity>(loader.map(ecs::to_main(*raw))));
				}
			}
		}
	}

	// What loader.get<T> did with the module's archive, with T's code reduced to `emplace`: every owner was made by the
	// entity pass of this loader (a new one per apply_commands), so it has no T to remove first. A value is remapped
	// before it is placed, so an on_construct observer sees the world's ids (scn_component_catalog.h). An owner the
	// loader has no world entity for is skipped: the sandbox holds a component only on a live entity, which the entity
	// pass maps; restore() of the loader would have made a new, empty world entity for it.
	void load_component_pool(entt::continuous_loader& loader, entt::registry& state, entt::id_type id,
		const entt::basic_sparse_set<ecs::sandbox_entity>& pool, const entt::meta_type& type, ecs::sandbox_emplace_fn emplace)
	{
		const bool remap = has_entity_fields(type);
		for (std::size_t i = 0; i < pool.size(); ++i) {
			const ecs::sandbox_entity owner = pool.data()[i];
			const entt::entity mapped = loader.map(ecs::to_main(owner));
			if (mapped == entt::null) {
				continue;
			}
			const void* value = pool.value(owner);
			if (remap && value) {
				// A copy of the value: copying a meta_any that refers to an object copies the object.
				const entt::meta_any sandbox_value = type.from_void(value);
				entt::meta_any copy = sandbox_value;
				if (copy) {
					remap_entity_fields(type, copy, loader);
					emplace(state, id, mapped, copy.base().data());
					continue;
				}
			}
			emplace(state, id, mapped, value);
		}
	}

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

	struct schema_pool
	{
		const sandbox_blob_pool* pool = nullptr;
		entt::id_type id = 0;
		const desc::schema_binding* schema = nullptr;
	};

	// After every pool is in, so an entity field may name a sandbox entity whose world counterpart a later pool made:
	// the loader makes the world entity of an owner only when it loads one of its pools.
	void map_schema_pool(entt::registry& state, const entt::continuous_loader& loader, const schema_pool& moved)
	{
		const loader_remap entities{ loader };
		auto& placed = state.storage<desc::dyn_blob>(moved.id);
		for (const ecs::sandbox_entity owner : static_cast<const entt::basic_sparse_set<ecs::sandbox_entity>&>(*moved.pool)) {
			const auto remote = static_cast<entt::entity>(entt::to_integral(owner));
			if (!loader.contains(remote)) {
				continue;
			}
			const entt::entity local = loader.map(remote);
			if (!placed.contains(local)) {
				continue;
			}
			desc::dyn_blob& blob = placed.get(local);
			// Of another size it was made against another layout of the schema: reading it by this one's offsets overruns
			// it. Known gap: a blob of another layout of the same size passes, and its bytes are read (and its "entities"
			// mapped) where this layout places them; the data is wrong, the memory stays in bounds (DESC_SCHEMA.md).
			desc::dyn_ref ref{ &blob, 0 };
			if (blob.bytes.size() != moved.schema->schema->layout.size || !desc::remap_entities(*moved.schema, &ref, entities)) {
				egLOG_ERROR("ecs", "a '{}' component from the commands is dropped: its blob is not laid out by the registered schema", moved.schema->schema->name);
				placed.remove(local);
			}
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
			sandbox_loader_archive archive{ loader, entity_storage };
			loader.get<entt::entity>(archive);
		}

		std::vector<schema_pool> schema_pools;
		for (auto [id, storage] : sandbox->storage()) {
			if (id == entt::type_hash<ecs::sandbox_entity>::value()) continue;
			if (storage.empty()) continue;

			if (auto type = ecs::find_meta(layer_meta, id)) {
				if (auto func = type.func(ecs::loader_get_h)) {
					const entt::meta_any adapter = func.invoke({});
					if (const auto* emplace = adapter.try_cast<ecs::sandbox_emplace_fn>(); emplace && *emplace) {
						load_component_pool(loader, state, id, storage, type, *emplace);
						continue;
					}
				}
			}
			if (catalog && storage.type() == entt::type_id<desc::dyn_blob>()) {
				if (const scn::component_entry* entry = catalog->find(id); entry && scn::is_schema_component(*entry)) {
					// Through the loader, as a C++ component goes: it makes the world entities of the owners.
					const auto& pool = static_cast<const sandbox_blob_pool&>(storage);
					sandbox_loader_archive archive{ loader, pool };
					loader.get<desc::dyn_blob>(archive, id);
					schema_pools.push_back({ &pool, id, entry->schema.get() });
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

		for (const schema_pool& moved : schema_pools) {
			map_schema_pool(state, loader, moved);
		}
		sandbox->clear();
	}
}
