#pragma once
#include "scn_world.h"
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <snk/ecs/ecs_random.hpp>
#include <snk/ecs/ecs_system_plan.hpp>
#include <functional>
#include <map>
#include <unordered_map>
#include <entt/entity/fwd.hpp>
#include "scn_world_desc.h"
#include "scn_hot_reload_manager.h"
#include <snk/common/eng_profiler.h>
#include <snk/common/snk_api.h>

namespace scn {
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
            swap(m_level_state, other.m_level_state);
            swap(m_stepper, other.m_stepper);
            swap(m_mode, other.m_mode);
            swap(m_seed, other.m_seed);
            swap(m_plans, other.m_plans);
            swap(m_phase_order, other.m_phase_order);
            swap(m_plan_slots, other.m_plan_slots);
            swap(m_worlds, other.m_worlds);
            swap(m_worlds_by_id, other.m_worlds_by_id);
            swap(m_world_systems, other.m_world_systems);
        }

        // One frame of ECS: the phases in order, fixed_update stepped by the accumulator, the
        // command buffer applied after every phase. What runs depends on the mode.
        void update(std::chrono::duration<float> dt);

        // Edit: only run_in_edit systems. Play: all (default). Pause: only run_in_edit systems with
        // time frozen (delta_time 0, no fixed steps). Step: the next update() runs exactly one
        // frame (one fixed step, the other phases once, delta_time = fixed step), then the mode
        // becomes Pause. The mode survives clear() and level loads.
        void set_mode(ecs::run_mode mode) { m_mode = mode; }
        ecs::run_mode mode() const { return m_mode; }

        // Seed of the level's random numbers. Every world owns an `ecs::rng` resource seeded from
        // it and its id (the level's own is reachable as level_res<ecs::rng>), so the same seed
        // reproduces the same sequences. Reseeds the existing worlds; worlds created later derive
        // from it as well.
        void set_seed(uint64_t seed);
        uint64_t seed() const { return m_seed; }

        scn::world& create_world(const std::string_view type, uint32_t world_id);

		std::size_t get_world_count() const { return m_worlds.size(); }
		bool has_world(const std::string_view name) const { return m_worlds.contains(std::string{ name }); }

		// Smallest non-negative world id not currently in use. Fills gaps left by remove_world,
		// so ids stay small. Prefer this over get_world_count() when picking an id for a new
		// world — count collides when ids are non-contiguous (e.g. after a remove).
		uint32_t acquire_free_world_id() const {
			uint32_t id = 0;
			while (m_worlds_by_id.contains(id)) ++id;
			return id;
		}

		template<typename Fn>
		void for_each_world(Fn&& fn) {
			for (auto& [name, world_ptr] : m_worlds) {
				fn(*world_ptr);
			}
		}

		template<typename Fn>
		void for_each_world(Fn&& fn) const {
			for (const auto& [name, world_ptr] : m_worlds) {
				fn(*world_ptr);
			}
		}

        scn::world& get_world(const std::string_view type) {
            ASSERT_MSG(m_worlds.contains(std::string{ type }), "level doesn't contain this type of world");
            return *m_worlds[std::string{ type }];
        }

        // Null when there is no such world: for callers holding a name that can outlive the world
        // (a level reload rebuilds the worlds from the desc and drops runtime-created ones).
        scn::world* find_world(const std::string_view name) const {
            auto it = m_worlds.find(std::string{ name });
            return it == m_worlds.end() ? nullptr : it->second.get();
        }

        scn::world& get_world(uint32_t id) const {
            ASSERT_MSG(m_worlds_by_id.contains(id), "level doesn't contain world with this ID");
            return *m_worlds_by_id.at(id);
        }

        entt::registry& state() { return m_level_state; }
        entt::registry const& state() const { return m_level_state; }

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
			auto it = m_plans.find(world_id);
			return it == m_plans.end() ? nullptr : &it->second;
		}

        void clear();

		const res::tag& get_tag() const { return m_current_level_tag; }

    private:
        void rebuild_world_plan(uint32_t world_id, ecs::system_factory& sf);
        // Takes the phase order from sf and re-resolves every world's phases against it.
        void refresh_plan_slots(const ecs::system_factory& sf);
        void run_phase(size_t order_index, ecs::run_mode mode);
        static void apply_commands(entt::registry& state);

        ecs::runtime_context create_runtime_context(size_t world_id) {
            ecs::runtime_context ctx{ .current_registry = get_world(world_id).state(), .current_world_id = world_id };
            ctx.get_world_registry = [this](size_t world_id) -> entt::registry& {
                if (world_id == LEVEL_ID) return m_level_state;
                return get_world(world_id).state();
            };
            return ctx;
        }
    private:
		res::tag m_current_level_tag;
		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
        entt::registry m_level_state;

        ecs::fixed_stepper m_stepper; // 60 Hz, dt clamped, steps per frame capped
        ecs::run_mode m_mode = ecs::run_mode::play;
        uint64_t m_seed = ecs::rng::default_seed;

        // Per world name: the system names the world runs (data from world_desc.systems).
        std::unordered_map<std::string, std::vector<std::string>> m_world_systems;
        // Per world id (ordered, so worlds always run in id order): the plan built from that list.
        std::map<uint32_t, ecs::system_plan> m_plans;
        // Phase order the level runs (copy of the table at the last plan build) and, per world id,
        // that world's phase_plan for every position of it (null where the plan has no such phase).
        // Resolved by phase id when plans are built, so a frame does no searching.
        std::vector<ecs::phase_id> m_phase_order = ecs::phase_table{}.ordered();
        std::map<uint32_t, std::vector<const ecs::phase_plan*>> m_plan_slots;
        std::unordered_map<std::string, std::unique_ptr<scn::world>> m_worlds;
        std::map<uint32_t, scn::world *> m_worlds_by_id;
    };
} // namespace scn
