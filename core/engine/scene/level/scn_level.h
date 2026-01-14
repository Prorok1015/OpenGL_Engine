#pragma once
#include "scn_world.h"
#include <functional>
#include <unordered_map>

namespace scn {
class level {
public:
  level() {
    // Initialize global level context
    m_level_state.ctx().emplace<level *>(this);

    // Callback for Invoker to find specific world registries
    using namespace entt::literals;

    // Callback for Invoker to find specific world registries
    // We register this in level_state context so each system callback can
    // retrieve it.
    m_level_state.ctx().emplace<std::function<entt::registry& (size_t)>>(std::function<entt::registry & (size_t)>{
        [this](size_t world_id) -> entt::registry& {
            return this->get_world((uint32_t)world_id).state();
            }
    });
  }

  ~level() = default;
  level(const level &) = delete;
  level(level &&) = default;
  level &operator=(const level &) = delete;
  level &operator=(level &&) = default;

  void update(std::chrono::duration<float> dt);

  scn::world &create_world(const std::string &type, uint32_t world_id) {
    auto &w = *(m_worlds[type] = std::make_unique<scn::world>(world_id));
    m_worlds_by_id[world_id] = &w; // Register in ID map

    // Set the WorldID in the world's own registry context
    using namespace entt::literals;
    w.state().ctx().emplace<ecs::world_index>((size_t)world_id);

    return w;
  }

  scn::world &get_world(const std::string &type) {
    ASSERT_MSG(m_worlds.contains(type),
               "level doesn't contain this type of world");
    return *m_worlds[type];
  }

  scn::world &get_world(uint32_t id) {
    ASSERT_MSG(m_worlds_by_id.contains(id),
               "level doesn't contain world with this ID");
    return *m_worlds_by_id[id];
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
      for (size_t top = 0; top < graph.size(); ++top)
      {
          auto& node = graph[top];
          if (node.top_level())
              run_graph_recursive(top, graph);
      }
  }

  void run_graph_recursive(size_t top, std::vector<entt::organizer::vertex>& graph)
  {
      auto& top_node = graph[top];
      top_node.prepare(m_level_state);
      top_node.callback()(top_node.data(), m_level_state);

      for (size_t out : top_node.out_edges()) {
          run_graph_recursive(out, graph);
      }
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