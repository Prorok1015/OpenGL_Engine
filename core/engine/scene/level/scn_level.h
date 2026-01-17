#pragma once
#include "scn_world.h"
#include <functional>
#include <unordered_map>
#include <entt/entity/fwd.hpp>

namespace scn {
    inline constexpr size_t LEVEL_ID = ~0u;
    template<class T>
    using level_res = ecs::bind_res<LEVEL_ID, T>;

    class level 
    {
    public:
        level() {
        m_level_state.ctx().emplace<ecs::runtime_context_provider>(
            ecs::runtime_context_provider::getter_type{
            [this](size_t world_id) { return this->create_runtime_context(world_id); }
        });
        }

        ~level() = default;
        level(const level &) = delete;
        level(level &&) = default;
        level &operator=(const level &) = delete;
        level &operator=(level &&) = default;

        void update(std::chrono::duration<float> dt);

        scn::world& create_world(const std::string &type, uint32_t world_id) {
            auto& w = *(m_worlds[type] = std::make_unique<scn::world>(world_id));
            m_worlds_by_id[world_id] = &w; // Register in ID map

            // Set the WorldID in the world's own registry context
            w.state().ctx().emplace<ecs::world_salt>((size_t)world_id);
            return w;
        }

        scn::world& get_world(const std::string &type) {
            ASSERT_MSG(m_worlds.contains(type), "level doesn't contain this type of world");
            return *m_worlds[type];
        }

        scn::world& get_world(uint32_t id) const {
            ASSERT_MSG(m_worlds_by_id.contains(id), "level doesn't contain world with this ID");
            return *m_worlds_by_id.at(id);
        }

        entt::organizer &organizer() { return m_organizer; }
        entt::organizer const &organizer() const { return m_organizer; }

        entt::registry& state() { return m_level_state; }
        entt::registry const& state() const { return m_level_state; }

        void mark_systems_graphs_dirty() {
            m_fixed_graph = m_fixed_organizer.graph();
            m_variable_graph = m_organizer.graph();
        }

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

        ecs::runtime_context create_runtime_context(size_t world_id) const {
            ecs::runtime_context ctx{ .current_registry = get_world(world_id).state(), .current_world_id = world_id };
            ctx.get_world_registry = [this](size_t world_id) -> entt::registry& {
                return get_world(world_id).state();
            };
            return ctx;
        }

    private:
        entt::registry m_level_state;

        float m_accumulator = 0.0f;
        const float m_fixed_step = 1.0f / 60.0f; // 60 Hz

        entt::organizer m_organizer;
        entt::organizer m_fixed_organizer;
        std::vector<entt::organizer::vertex> m_fixed_graph;
        std::vector<entt::organizer::vertex> m_variable_graph;
        std::unordered_map<std::string, std::unique_ptr<scn::world>> m_worlds;
        std::unordered_map<uint32_t, scn::world *> m_worlds_by_id;
    };
} // namespace scn