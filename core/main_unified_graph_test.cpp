#include "core/ecs/ecs_system.h"
#include "engine/scene/level/scn_level.h"
#include <entt/entt.hpp>
#include <iostream>
#include <string>

// Components
struct Health {
  float value;
};
struct Text {
  std::string content;
};
struct CtxText
{
    std::string text;
};

// System Logic
// Reads Health from World 0, Writes to Text in Current World (UI)
void sync_health_bar(
    entt::view<entt::get_t<Text>> view,      // Local (UI world)
    const scn::bind_res<0, Health> player_hp, // From world 0 (Game world)
    const CtxText& text
) {
  float hp = player_hp->value;
  std::cout << "[System] Syncing Health: " << hp << std::endl;

  view.each([hp](auto entity, auto &text) {
    text.content = "HP: " + std::to_string(hp);
    std::cout << "[System] Updated Entity " << (uint32_t)entity
              << " Text to: " << text.content << std::endl;
  });
  std::cout << "[System] ctx text: " << text.text << std::endl;
}

void ctx_text_unpdate(
    const CtxText& text
)
{
    static int id = 0;
    //text.text = std::format("text update id:{}", id++);
    std::cout << "[Update] " << text.text << std::endl;
}

int main() {
  using namespace entt::literals;

  // 1. Create Level
  scn::level lvl;
  ecs::system_factory &factory = ecs::system_factory::instance();

  // 2. Create Worlds
  auto &game_world = lvl.create_world("game", 0);
  auto &ui_world = lvl.create_world("ui", 1);

  // 3. Register System
  // We register the system logic in the factory
  factory.register_automatic_system<&sync_health_bar>("ui::sync_health");
  factory.register_automatic_system<&ctx_text_unpdate>("ui::update");

  // 4. Instantiate System in UI World
  // The factory creates the graph node in the Level's organizer
  factory.create_system("ui::sync_health", ui_world.state(), lvl.organizer());
  factory.create_system("ui::update", ui_world.state(), lvl.organizer());

  // 5. Populate Data
  // Game World: Player with Health
  {
    // Add single "Player" entity or Context Variable?
    // The system asks for `WorldRes`. In our Invoker implementation, bind_res
    // maps to Context Variable. So we add Health to World 0 context.
    game_world.state().ctx().emplace<Health>(100.0f);
    ui_world.state().ctx().emplace<CtxText>("text update id: index");
  }

  // UI World: Text Entity
  {
    auto e = ui_world.state().create();
    ui_world.state().emplace<Text>(e, "HP: ???");
  }

  // 6. Build Graph
  // Normally level updates do this?
  // scn::level::mark_systems_graphs_dirty does this.
  lvl.mark_systems_graphs_dirty();

  // 7. Run Frame
  std::cout << "--- Frame 1 ---" << std::endl;
  lvl.update(std::chrono::milliseconds(16));

  // Change Health
  game_world.state().ctx().get<Health>().value = 50.0f;

  std::cout << "--- Frame 2 ---" << std::endl;
  lvl.update(std::chrono::milliseconds(16));

  return 0;
}
