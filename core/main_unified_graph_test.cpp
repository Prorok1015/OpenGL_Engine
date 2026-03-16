#include "core/ecs/ecs_system.h"
#include "core/ecs/ecs_component.h"
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

struct SubText {
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
    entt::view<entt::get_t<SubText>> subview,
    ecs::bind_res<0, Health> player_hp, // From world 0 (Game world)
    const CtxText& text
) {
  float hp = player_hp->value;
  std::cout << "[System] Syncing Health: " << hp << std::endl;

  view.each([hp](auto entity, auto &text) {
    std::cout << "[System] Updated Entity " << (uint32_t)entity << " Text to: " << text.content << std::endl;
  });

  subview.each([hp](auto entity, auto &text) {
    std::cout << "[System] sub text " << (uint32_t)entity << " Text to: " << text.content << std::endl;
  });

  std::cout << "[System] ctx text: " << text.text << std::endl;
}

void ctx_text_unpdate(CtxText& text, ecs::entity_spawner& sandbox)
{
    static int id = 0;
    text.text = std::format("text update id:{}", id++);
    sandbox.emplace<SubText>(sandbox.create(), std::format("I amd a sub new text {}", id));
    sandbox.emplace<Text>(sandbox.create(), std::format("I amd a new text {}", id));
}

void ctx_text_read(entt::view<entt::get_t<Text>, entt::exclude_t<SubText>> view, ecs::entity_changer& changer)
{
    for (auto&& [ent, text] : view.each())
    {
        changer.destroy(ent);
    }
}

struct sys : ecs::system_interface {
    sys(std::string str)
        : text(str) {}

    void call(entt::view<entt::get_t<SubText>> subview) {

        subview.each([this] (auto entity, auto& text) {
            std::cout << "[Embedded " << this->text << " System] sub text " << (uint32_t)entity << " Text to : " << text.content << std::endl;
        });
    }

    std::string text;
};

int main() {
  using namespace entt::literals;

  // 1. Create Level
  scn::level lvl;
  ecs::system_factory factory;
  auto system1 = std::make_shared<sys>("instance"s);
  // 2. Create Worlds
  auto &game_world = lvl.create_world("game", 0);
  auto &ui_world = lvl.create_world("ui", 1);

  ecs::register_component<Text>("text");
  ecs::register_component<SubText>("subtext");

  // 3. Register System
  // We register the system logic in the factory
  factory.register_automatic_system<&sys::call>("sys::embedded", system1);
  factory.register_automatic_system<&sys::call, sys>("sys::embedded2", "in place"s);
  factory.register_automatic_system<&sync_health_bar>("ui::sync_health");
  factory.register_automatic_system<&ctx_text_unpdate>("ui::update");
  factory.register_automatic_system<&ctx_text_read>("ui::read");

  // 4. Instantiate System in UI World
  // The factory creates the graph node in the Level's organizer
  factory.create_system("ui::sync_health", ui_world.state(), lvl.organizer());
  factory.create_system("ui::read", ui_world.state(), lvl.organizer());
  factory.create_system("ui::update", ui_world.state(), lvl.organizer());
  factory.create_system("sys::embedded", ui_world.state(), lvl.organizer());
  auto p = factory.create_system("sys::embedded2", ui_world.state(), lvl.organizer());

  // 5. Populate Data
  // Game World: Player with Health
  {
    // Add single "Player" entity or Context Variable?
    // The system asks for `WorldRes`. In our invoker implementation, bind_res
    // maps to Context Variable. So we add Health to World 0 context.
    game_world.state().ctx().emplace<Health>(100.0f);
  }

  // UI World: Text Entity
  {
    auto e = ui_world.state().create();
    ui_world.state().emplace<Text>(e, "HP: ???");
    ui_world.state().ctx().emplace<CtxText>("text update id: index");
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

  // Change Health
  game_world.state().ctx().get<Health>().value = 0.0f;

  std::cout << "--- Frame 3 ---" << std::endl;
  lvl.update(std::chrono::milliseconds(16));

  return 0;
}
