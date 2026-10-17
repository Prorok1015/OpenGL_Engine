#pragma once
#include "scn_world.h"
#include <functional>
#include <unordered_map>
#include <entt/entity/fwd.hpp>
#include "scn_world_desc.h"
#include "scn_hot_reload_manager.h"
#include "eng_profiler.h"

namespace scn {
    inline constexpr size_t LEVEL_ID = ~0u;
    template<class T>
    using level_res = ecs::bind_res<LEVEL_ID, T>;

    class level 
    {
    public:
        level(std::shared_ptr<scn::hot_reload_manager> hot_manager);

        ~level();
        level(const level &) = delete;
        level(level &&other) noexcept : level(other.m_hot_reload_manager) { swap(other); }
        level &operator=(const level &) = delete;
        level &operator=(level &&other) noexcept { swap(other); return *this; }

		// The runtime context provider captures `this`, so it has to be re-installed on both sides.
		void swap(level& other) noexcept {
            using std::swap;
            swap(m_current_level_tag, other.m_current_level_tag);
            swap(m_hot_reload_manager, other.m_hot_reload_manager);
            swap(m_level_state, other.m_level_state);
            swap(m_accumulator, other.m_accumulator);
            swap(m_organizer, other.m_organizer);
            swap(m_fixed_organizer, other.m_fixed_organizer);
            swap(m_fixed_graph, other.m_fixed_graph);
            swap(m_variable_graph, other.m_variable_graph);
            swap(m_fixed_order, other.m_fixed_order);
            swap(m_variable_order, other.m_variable_order);
            swap(m_worlds, other.m_worlds);
            swap(m_worlds_by_id, other.m_worlds_by_id);
            swap(m_world_systems, other.m_world_systems);
            swap(m_system_handles, other.m_system_handles);
            install_context_provider();
            other.install_context_provider();
        }

        void update(std::chrono::duration<float> dt);

        scn::world& create_world(const std::string_view type, uint32_t world_id);

		std::size_t get_world_count() const { return m_worlds.size(); }
		bool has_world(const std::string_view name) const { return m_worlds.contains(std::string{ name }); }

		// Smallest non-negative world id not currently in use. Fills gaps left by remove_world,
		// so ids stay small (the automatic system dispatcher supports world ids 0-4). Prefer this
		// over get_world_count() when picking an id for a new world — count collides when ids are
		// non-contiguous (e.g. after a remove).
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

        scn::world& get_world(uint32_t id) const {
            ASSERT_MSG(m_worlds_by_id.contains(id), "level doesn't contain world with this ID");
            return *m_worlds_by_id.at(id);
        }

        entt::organizer &organizer() { return m_organizer; }
        entt::organizer const &organizer() const { return m_organizer; }

        entt::registry& state() { return m_level_state; }
        entt::registry const& state() const { return m_level_state; }

		void load_from_desc(const level_desc& desc, ecs::system_factory& desc_system, scn::ecs_assembler& assambler);

		void reload_world(
			const std::string_view name,
			const world_desc& desc,
			ecs::system_factory& sf,
			scn::ecs_assembler& assambler);

		// Removes the world together with its systems; the organizer is rebuilt from the remaining worlds.
		void remove_world(const std::string_view name, ecs::system_factory& sf);

		// Remembers the world's system list and instantiates those systems into the organizer.
		void add_world_systems(
			scn::world& world,
			const std::vector<std::string>& systems,
			ecs::system_factory& sf);

		void load_systems_from_desc(const level_desc& desc, ecs::system_factory& desc_system);

        void mark_systems_graphs_dirty() {
            m_fixed_graph = m_fixed_organizer.graph();
            m_variable_graph = m_organizer.graph();
            m_fixed_order = topological_order(m_fixed_graph);
            m_variable_order = topological_order(m_variable_graph);
        }

        void clear_organizer()
        {
            m_organizer.clear();
            m_fixed_organizer.clear();
            m_fixed_graph.clear();
            m_variable_graph.clear();
            m_fixed_order.clear();
            m_variable_order.clear();
            m_system_handles.clear();
		}

        void clear();

		const res::tag& get_tag() const { return m_current_level_tag; }

		// Kahn's algorithm; ties are broken by the smallest vertex index so the order is deterministic.
		static std::vector<size_t> topological_order(const std::vector<entt::organizer::vertex>& graph);
    private:
        void run_graph(std::vector<entt::organizer::vertex> &graph, const std::vector<size_t>& order) {
            for (size_t idx : order) {
                auto& node = graph[idx];
                PROFILE_SCOPE(node.name());
                node.prepare(m_level_state);
                node.callback()(node.data(), m_level_state);
            }
        }

        void install_context_provider() {
            m_level_state.ctx().insert_or_assign(ecs::runtime_context_provider{
                ecs::runtime_context_provider::getter_type{
                    [this](size_t world_id) { return this->create_runtime_context(world_id); } } });
        }

        void create_world_systems(scn::world& world, ecs::system_factory& sf);
        void rebuild_systems(ecs::system_factory& sf);

        ecs::runtime_context create_runtime_context(size_t world_id) {
            ecs::runtime_context ctx{ .current_registry = get_world(world_id).state(), .current_world_id = world_id };
            ctx.get_world_registry = [this](size_t world_id) -> entt::registry& {
                if (world_id == LEVEL_ID) return m_level_state;
                return get_world(world_id).state();
            };
            return ctx;
        }

        void sync_point();
    private:
		res::tag m_current_level_tag;
		std::shared_ptr<scn::hot_reload_manager> m_hot_reload_manager;
        entt::registry m_level_state;

        float m_accumulator = 0.0f;
        static constexpr float m_fixed_step = 1.0f / 60.0f; // 60 Hz

        entt::organizer m_organizer;
        entt::organizer m_fixed_organizer;
        std::vector<entt::organizer::vertex> m_fixed_graph;
        std::vector<entt::organizer::vertex> m_variable_graph;
        std::vector<size_t> m_fixed_order;
        std::vector<size_t> m_variable_order;
        // Per world name: the system names to instantiate and the instances the organizer points into.
        std::unordered_map<std::string, std::vector<std::string>> m_world_systems;
        std::unordered_map<std::string, std::vector<std::shared_ptr<ecs::system_interface>>> m_system_handles;
        std::unordered_map<std::string, std::unique_ptr<scn::world>> m_worlds;
        std::unordered_map<uint32_t, scn::world *> m_worlds_by_id;
    };
} // namespace scn