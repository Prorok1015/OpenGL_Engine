#pragma once
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/common/eng_profiler.h>
#include <snk/common/snk_api.h>
#include <functional>
#include <utility>

namespace scn {
    // Thin facade over one context_set. Built standalone it owns its set; the context_manager
    // hands out a facade over its "level" set. Goes away when the callers move to the set (PR-6).
    class SNK_API level
    {
    public:
        explicit level(std::shared_ptr<scn::hot_reload_manager> hot_manager);
        level(scn::context_set& set, std::shared_ptr<scn::hot_reload_manager> hot_manager);

        ~level();
        level(const level &) = delete;
        level(level &&other) noexcept : level(other.m_hot_reload_manager) { swap(other); }
        level &operator=(const level &) = delete;
        level &operator=(level &&other) noexcept { swap(other); return *this; }

		void swap(level& other) noexcept {
            using std::swap;
            swap(m_hot_reload_manager, other.m_hot_reload_manager);
            swap(m_owned, other.m_owned);
            swap(m_set, other.m_set);
        }

        // The set's frame, then the hot reload tick.
        void update(std::chrono::duration<float> dt);

        // See context_set::set_mode.
        void set_mode(ecs::run_mode mode) { m_set->set_mode(mode); }
        ecs::run_mode mode() const { return m_set->mode(); }

        // See context_set::set_seed.
        void set_seed(uint64_t seed) { m_set->set_seed(seed); }
        uint64_t seed() const { return m_set->seed(); }

        scn::world& create_world(const std::string_view type, uint32_t world_id, context_kind kind = context_kind::runtime) {
            return m_set->create_context(type, world_id, kind);
        }

		std::size_t get_world_count() const { return m_set->size(); }
		bool has_world(const std::string_view name) const { return m_set->has(name); }

		// Smallest non-negative world id not currently in use. Fills gaps left by remove_world,
		// so ids stay small. Prefer this over get_world_count() when picking an id for a new
		// world — count collides when ids are non-contiguous (e.g. after a remove).
		uint32_t acquire_free_world_id() const { return m_set->acquire_free_id(); }

		template<typename Fn>
		void for_each_world(Fn&& fn) { m_set->for_each(std::forward<Fn>(fn)); }

		template<typename Fn>
		void for_each_world(Fn&& fn) const { std::as_const(*m_set).for_each(std::forward<Fn>(fn)); }

        scn::world& get_world(const std::string_view type) { return m_set->get(type); }

        // Null when there is no such world: for callers holding a name that can outlive the world
        // (a level reload rebuilds the worlds from the desc and drops runtime-created ones).
        scn::world* find_world(const std::string_view name) const { return m_set->find(name); }

        scn::world& get_world(uint32_t id) const { return m_set->get(id); }

		void load_from_desc(const level_desc& desc, ecs::system_factory& desc_system, scn::ecs_assembler& assambler) {
			m_set->load_from_desc(desc, desc_system, assambler);
		}

		void reload_world(
			const std::string_view name,
			const world_desc& desc,
			ecs::system_factory& sf,
			scn::ecs_assembler& assambler) {
			m_set->reload_context(name, desc, sf, assambler);
		}

		// Removes the world together with its systems and execution plan.
		void remove_world(const std::string_view name) { m_set->destroy_context(name); }

		void add_world_systems(
			scn::world& world,
			const std::vector<std::string>& systems,
			ecs::system_factory& sf) {
			m_set->add_systems(world, systems, sf);
		}

		// See context_set::load_systems_from_desc for the reload contract.
		void load_systems_from_desc(const level_desc& desc, ecs::system_factory& desc_system) {
			m_set->load_systems_from_desc(desc, desc_system);
		}

		void rebuild_all_plans(ecs::system_factory& sf) { m_set->rebuild_all_plans(sf); }

		// Plan of a world, null when the world has no systems registered.
		const ecs::system_plan* plan_of(uint32_t world_id) const { return m_set->plan_of(world_id); }

        void clear() { m_set->clear(); }

		const res::tag& get_tag() const { return m_set->tag(); }

		scn::context_set& set() { return *m_set; }
		const scn::context_set& set() const { return *m_set; }

    private:
		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
		std::unique_ptr<scn::context_set> m_owned;
		scn::context_set* m_set = nullptr;
    };
} // namespace scn
