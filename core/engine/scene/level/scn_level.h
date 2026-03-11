#pragma once
#include "scn_world.h"
#include <functional>
#include <unordered_map>
#include <entt/entity/fwd.hpp>
#include "scn_world_desc.h"
#include "scn_hot_reload_manager.h"

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
        level(level &&) noexcept = default;
        level &operator=(const level &) = delete;
        level &operator=(level &&) noexcept = default;

		void swap(level& other) noexcept {
            using std::swap;
            swap(m_level_state, other.m_level_state);
            swap(m_accumulator, other.m_accumulator);
            swap(m_organizer, other.m_organizer);
            swap(m_fixed_organizer, other.m_fixed_organizer);
            swap(m_fixed_graph, other.m_fixed_graph);
            swap(m_variable_graph, other.m_variable_graph);
            swap(m_worlds, other.m_worlds);
            swap(m_worlds_by_id, other.m_worlds_by_id);
        }

        void update(std::chrono::duration<float> dt);

        scn::world& create_world(const std::string_view type, uint32_t world_id);

		std::size_t get_world_count() const { return m_worlds.size(); }

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

		void load_world_from_desc(const world_desc& desc, ecs::system_factory& desc_system, scn::ecs_assembler& assambler);

		void load_systems_from_desc(const level_desc& desc, ecs::system_factory& desc_system);

        void mark_systems_graphs_dirty() {
            m_fixed_graph = m_fixed_organizer.graph();
            m_variable_graph = m_organizer.graph();
        }

        void clear_organizer()
        {
            m_organizer.clear();
            m_fixed_organizer.clear();
            m_fixed_graph.clear();
            m_variable_graph.clear();
		}

        void clear();

		const res::tag& get_tag() const { return m_current_level_tag; }
    private:
        void run_graph(std::vector<entt::organizer::vertex> &graph) {
            for (size_t top = 0; top < graph.size(); ++top) {
                auto& node = graph[top];
                if (node.top_level())
                    run_graph_recursive(top, graph);
            }
        }

        void run_graph_recursive(size_t top, std::vector<entt::organizer::vertex>& graph) {
            auto& top_node = graph[top];
            top_node.prepare(m_level_state);
            top_node.callback()(top_node.data(), m_level_state);

            for (size_t out : top_node.out_edges()) {
                run_graph_recursive(out, graph);
            }
        }

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
        std::unordered_map<std::string, std::unique_ptr<scn::world>> m_worlds;
        std::unordered_map<uint32_t, scn::world *> m_worlds_by_id;
    };
} // namespace scn