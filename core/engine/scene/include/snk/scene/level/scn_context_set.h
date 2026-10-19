#pragma once
#include <snk/scene/level/scn_world.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <snk/ecs/ecs_invoker.hpp>
#include <snk/ecs/ecs_random.hpp>
#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/resource/res_tag.h>
#include <snk/common/engine_assert.h>
#include <snk/common/snk_api.h>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scn
{
	// Resolves to the registry of the context the system runs in.
	inline constexpr size_t LEVEL_ID = ~0u;
	template<class T>
	using level_res = ecs::bind_res<LEVEL_ID, T>;

	// A group of contexts stepped together: one mode, one seed, one phase order. Owned by the
	// context_manager (the level is the set "level"); a standalone scn::level owns one as well.
	// A context keeps its address until it is destroyed.
	class SNK_API context_set
	{
	public:
		context_set(std::string name, std::shared_ptr<scn::hot_reload_manager> hot_manager);
		~context_set();
		context_set(const context_set&) = delete;
		context_set& operator=(const context_set&) = delete;
		context_set(context_set&&) = delete;
		context_set& operator=(context_set&&) = delete;

		const std::string& name() const { return m_name; }
		const res::tag& tag() const { return m_tag; }
		// An inactive set is skipped by context_manager::update.
		bool active() const { return m_active; }
		void set_active(bool active) { m_active = active; }

		// One frame of ECS for every context: the phases in order, fixed_update stepped by the
		// accumulator, the command buffer applied after every phase. What runs depends on the mode.
		void update(std::chrono::duration<float> dt);

		// Edit: only run_in_edit systems. Play: all (default). Pause: only run_in_edit systems with
		// time frozen. Step: the next update() runs exactly one frame, then the mode becomes Pause.
		// The mode survives clear() and level loads.
		void set_mode(ecs::run_mode mode);
		ecs::run_mode mode() const { return m_mode; }

		// Every context owns an `ecs::rng` seeded from this seed and its id. Reseeds the existing
		// contexts; contexts created later derive from it as well.
		void set_seed(uint64_t seed);
		uint64_t seed() const { return m_seed; }

		// A context created into a non-empty set joins at the same point of the step as the others.
		scn::world_context& create_context(std::string_view name, uint32_t id, context_kind kind = context_kind::runtime);
		void destroy_context(std::string_view name);

		std::size_t size() const { return m_contexts.size(); }
		bool has(std::string_view name) const { return m_context_ids.contains(std::string{ name }); }

		// Smallest non-negative id not currently in use. Prefer this over size() when picking an id
		// for a new context: size() collides when ids are non-contiguous (after a destroy).
		uint32_t acquire_free_id() const {
			uint32_t id = 0;
			while (m_contexts.contains(id)) ++id;
			return id;
		}

		// In id order.
		template<typename Fn>
		void for_each(Fn&& fn) {
			for (const auto& [id, context] : m_contexts) {
				fn(*context);
			}
		}

		template<typename Fn>
		void for_each(Fn&& fn) const {
			for (const auto& [id, context] : m_contexts) {
				fn(*context);
			}
		}

		scn::world_context& get(std::string_view name) {
			auto it = m_context_ids.find(std::string{ name });
			ASSERT_MSG(it != m_context_ids.end(), "level doesn't contain this type of world");
			return *m_contexts.at(it->second);
		}

		// Null when there is no such context: for callers holding a name that can outlive it (a
		// level reload rebuilds the contexts from the desc and drops runtime-created ones).
		scn::world_context* find(std::string_view name) const {
			auto it = m_context_ids.find(std::string{ name });
			return it == m_context_ids.end() ? nullptr : m_contexts.at(it->second).get();
		}

		scn::world_context& get(uint32_t id) const {
			auto it = m_contexts.find(id);
			ASSERT_MSG(it != m_contexts.end(), "level doesn't contain world with this ID");
			return *it->second;
		}

		// Names of the worlds of the last loaded level desc, in desc order. Contexts created later
		// are not part of it.
		const std::vector<std::string>& desc_world_names() const { return m_desc_world_names; }

		// One `kind::level` context per world of the desc.
		void load_from_desc(const level_desc& desc, ecs::system_factory& desc_system, scn::ecs_assembler& assambler);

		// Recreates the context under the same id and kind.
		void reload_context(
			std::string_view name,
			const world_desc& desc,
			ecs::system_factory& sf,
			scn::ecs_assembler& assambler);

		// Remembers the context's system list and builds its execution plan (once, not per frame).
		void add_systems(
			scn::world_context& context,
			const std::vector<std::string>& systems,
			ecs::system_factory& sf);

		// The system list of the contexts changed (hot reload): rebuilds every plan.
		// Reload contract (systems only): per-context system instances (registered in place, with
		// ARGS...) are re-created, so they pick up new parameters and lose their runtime state;
		// shared instances (registered as a shared_ptr) are kept as they are. Mode, seed, the
		// fixed-step accumulator and the random streams are not touched; only a full
		// load_from_desc / reload_context re-seeds the streams.
		void load_systems_from_desc(const level_desc& desc, ecs::system_factory& desc_system);

		// The phase table of `sf` changed after plans were built (a game phase was added): rebuilds
		// every plan so systems of the new phase run and every context agrees on the phase order.
		// Game phases should be registered before the first load; this is the fallback.
		// Per-context system instances are re-created, like in load_systems_from_desc.
		void rebuild_all_plans(ecs::system_factory& sf);

		// Plan of a context, null when it has no systems registered.
		const ecs::system_plan* plan_of(uint32_t id) const {
			auto it = m_contexts.find(id);
			return it == m_contexts.end() ? nullptr : it->second->plan();
		}

		// Destroys every context; mode and seed stay.
		void clear();

	private:
		void rebuild_context_plan(uint32_t id, ecs::system_factory& sf);
		// Takes the phase order from sf and re-resolves every context's phases against it.
		void refresh_plan_slots(const ecs::system_factory& sf);
		// fixed_step >= 0: one fixed step, only contexts that still have a step to play take part.
		void run_phase(size_t order_index, ecs::run_mode mode, int fixed_step = -1);
		void destroy_by_id(uint32_t id);
		// Contexts keep their own mode; the set only mirrors its mode into them.
		void sync_context_mode();
		ecs::runtime_context create_runtime_context(size_t own_id);

		std::string m_name;
		res::tag m_tag;
		std::vector<std::string> m_desc_world_names;
		bool m_active = true;
		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;

		ecs::run_mode m_mode = ecs::run_mode::play;
		uint64_t m_seed = ecs::rng::default_seed;

		// Phase order the set runs (copy of the table at the last plan build). Each context
		// resolves its plan against it when plans are built, so a frame does no searching.
		std::vector<ecs::phase_id> m_phase_order = ecs::phase_table{}.ordered();
		// Ordered by id, so contexts always run in id order.
		std::map<uint32_t, std::unique_ptr<scn::world_context>> m_contexts;
		std::unordered_map<std::string, uint32_t> m_context_ids;
	};
}
