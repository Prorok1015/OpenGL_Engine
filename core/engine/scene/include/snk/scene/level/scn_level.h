#pragma once
#include <snk/scene/level/scn_world.h>
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <snk/ecs/ecs_random.hpp>
#include <snk/ecs/ecs_system_plan.hpp>
#include <functional>
#include <map>
#include <unordered_map>
#include <entt/entity/fwd.hpp>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/common/eng_profiler.h>
#include <snk/common/snk_api.h>

namespace scn {
    // Resolves to the registry of the context the system runs in.
    inline constexpr size_t LEVEL_ID = ~0u;
    template<class T>
    using level_res = ecs::bind_res<LEVEL_ID, T>;

    class SNK_API level
    {
    public:
        level(std::shared_ptr<scn::hot_reload_manager> hot_manager);

        ~level();
        level(const level &) = delete;
        level(level &&other) noexcept : level(other.m_hot_reload_manager) { swap(other); }
        level &operator=(const level &) = delete;
        level &operator=(level &&other) noexcept { swap(other); return *this; }

		void swap(level& other) noexcept {
            using std::swap;
            swap(m_current_level_tag, other.m_current_level_tag);
            swap(m_hot_reload_manager, other.m_hot_reload_manager);
            swap(m_mode, other.m_mode);
            swap(m_seed, other.m_seed);
            swap(m_phase_order, other.m_phase_order);
            swap(m_contexts, other.m_contexts);
            swap(m_context_ids, other.m_context_ids);
        }

        // One frame of ECS: the phases in order, fixed_update stepped by the accumulator, the
        // command buffer applied after every phase. What runs depends on the mode.
        void update(std::chrono::duration<float> dt);

        // Edit: only run_in_edit systems. Play: all (default). Pause: only run_in_edit systems with
        // time frozen (delta_time 0, no fixed steps). Step: the next update() runs exactly one
        // frame (one fixed step, the other phases once, delta_time = fixed step), then the mode
        // becomes Pause. The mode survives clear() and level loads.
        void set_mode(ecs::run_mode mode);
        ecs::run_mode mode() const { return m_mode; }

        // Seed of the level's random numbers. Every world owns an `ecs::rng` resource seeded from
        // it and its id, so the same seed
        // reproduces the same sequences. Reseeds the existing worlds; worlds created later derive
        // from it as well.
        void set_seed(uint64_t seed);
        uint64_t seed() const { return m_seed; }

        scn::world& create_world(const std::string_view type, uint32_t world_id);

		std::size_t get_world_count() const { return m_contexts.size(); }
		bool has_world(const std::string_view name) const { return m_context_ids.contains(std::string{ name }); }

		// Smallest non-negative world id not currently in use. Fills gaps left by remove_world,
		// so ids stay small. Prefer this over get_world_count() when picking an id for a new
		// world — count collides when ids are non-contiguous (e.g. after a remove).
		uint32_t acquire_free_world_id() const {
			uint32_t id = 0;
			while (m_contexts.contains(id)) ++id;
			return id;
		}

		template<typename Fn>
		void for_each_world(Fn&& fn) {
			for (const auto& [name, id] : m_context_ids) {
				fn(*m_contexts.at(id));
			}
		}

		template<typename Fn>
		void for_each_world(Fn&& fn) const {
			for (const auto& [name, id] : m_context_ids) {
				fn(*m_contexts.at(id));
			}
		}

        scn::world& get_world(const std::string_view type) {
            auto it = m_context_ids.find(std::string{ type });
            ASSERT_MSG(it != m_context_ids.end(), "level doesn't contain this type of world");
            return *m_contexts.at(it->second);
        }

        // Null when there is no such world: for callers holding a name that can outlive the world
        // (a level reload rebuilds the worlds from the desc and drops runtime-created ones).
        scn::world* find_world(const std::string_view name) const {
            auto it = m_context_ids.find(std::string{ name });
            return it == m_context_ids.end() ? nullptr : m_contexts.at(it->second).get();
        }

        scn::world& get_world(uint32_t id) const {
            auto it = m_contexts.find(id);
            ASSERT_MSG(it != m_contexts.end(), "level doesn't contain world with this ID");
            return *it->second;
        }

		void load_from_desc(const level_desc& desc, ecs::system_factory& desc_system, scn::ecs_assembler& assambler);

		void reload_world(
			const std::string_view name,
			const world_desc& desc,
			ecs::system_factory& sf,
			scn::ecs_assembler& assambler);

		// Removes the world together with its systems and execution plan.
		void remove_world(const std::string_view name);

		// Remembers the world's system list and builds its execution plan (once, not per frame).
		void add_world_systems(
			scn::world& world,
			const std::vector<std::string>& systems,
			ecs::system_factory& sf);

		// The system list of the worlds changed (hot reload): rebuilds every world's plan.
		// Reload contract (systems only): per-world system instances (registered in place, with
		// ARGS...) are re-created, so they pick up new parameters and lose their runtime state;
		// shared instances (registered as a shared_ptr) are kept as they are. Mode, seed, the
		// fixed-step accumulator and the worlds' random streams are not touched; only a full
		// load_from_desc / reload_world re-seeds the streams.
		void load_systems_from_desc(const level_desc& desc, ecs::system_factory& desc_system);

		// The phase table of `sf` changed after plans were built (a game phase was added): rebuilds
		// every world's plan so systems of the new phase run and every world agrees on the phase
		// order. Game phases should be registered before the first load; this is the fallback.
		// Per-world system instances are re-created, like in load_systems_from_desc.
		void rebuild_all_plans(ecs::system_factory& sf);

		// Plan of a world, null when the world has no systems registered.
		const ecs::system_plan* plan_of(uint32_t world_id) const {
			auto it = m_contexts.find(world_id);
			return it == m_contexts.end() ? nullptr : it->second->plan();
		}

        void clear();

		const res::tag& get_tag() const { return m_current_level_tag; }

    private:
        void rebuild_world_plan(uint32_t world_id, ecs::system_factory& sf);
        // Takes the phase order from sf and re-resolves every world's phases against it.
        void refresh_plan_slots(const ecs::system_factory& sf);
        // fixed_step >= 0: one fixed step, only contexts that still have a step to play take part.
        void run_phase(size_t order_index, ecs::run_mode mode, int fixed_step = -1);
        void destroy_context(uint32_t id);
        // Contexts keep their own mode; the level only mirrors its mode into them.
        void sync_context_mode();

        ecs::runtime_context create_runtime_context(size_t own_id) {
            ecs::runtime_context ctx{ .current_registry = get_world(own_id).state(), .current_world_id = own_id };
            ctx.get_world_registry = [this, own_id](size_t world_id) -> entt::registry& {
                if (world_id == LEVEL_ID) return get_world(own_id).state();
                return get_world(world_id).state();
            };
            return ctx;
        }
    private:
		res::tag m_current_level_tag;
		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;

        ecs::run_mode m_mode = ecs::run_mode::play;
        uint64_t m_seed = ecs::rng::default_seed;

        // Phase order the level runs (copy of the table at the last plan build). Each context
        // resolves its plan against it when plans are built, so a frame does no searching.
        std::vector<ecs::phase_id> m_phase_order = ecs::phase_table{}.ordered();
        // Ordered by id, so contexts always run in id order.
        std::map<uint32_t, std::unique_ptr<scn::world_context>> m_contexts;
        std::unordered_map<std::string, uint32_t> m_context_ids;
    };
} // namespace scn
