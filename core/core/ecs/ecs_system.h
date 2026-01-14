#pragma once
#include <entt/entt.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Use relative paths to be safe
#include "engine_assert.h"
#include "logger/engine_log.h"

#include "ecs_invoker.hpp" // Has Invoker, runtime_context
#include "ecs_system_interface.hpp"
#include "ecs_traits.hpp" // Has salted_type, function_traits

// --- Specialization of type_hash for salted_type Types ---
namespace entt {
template <typename T, size_t ID> struct type_hash<ecs::salted_type<T, ID>> {
  [[nodiscard]] static constexpr id_type value() noexcept {
    // Salt the hash: Original ^ (ID << 16)
    return type_hash<T>::value() ^ (static_cast<id_type>(ID) << 16);
  }
};

// Support salted_type<const T> as well
template <typename T, size_t ID> struct type_hash<ecs::salted_type<const T, ID>> {
  [[nodiscard]] static constexpr id_type value() noexcept {
    return type_hash<T>::value() ^ (static_cast<id_type>(ID) << 16);
  }
};
} // namespace entt

namespace ecs {

// --- System Factory ---
class system_factory {
public:
  // Callback signature: register(world_registry, organizer)
  using SystemInitializer = std::function<std::unique_ptr<system_interface>( entt::registry &, entt::organizer &)>;

  static system_factory &instance() {
    static system_factory inst;
    return inst;
  }

  // --- Core Dispatcher Logic for Automatic Systems ---
  template <auto Candidate, size_t WorldID, typename... SaltedReqs>
  static void emplace_automatic_system(const char *name, entt::organizer &org, type_list<SaltedReqs...>) {
    org.emplace<SaltedReqs...>(&InvokerCallback<Candidate>, reinterpret_cast<const void *>(WorldID), name);
  }

  // The generic callback executed by Organizer
  template <auto Candidate>
  static void InvokerCallback(const void *payload, entt::registry &level_registry) {
    size_t world_id = reinterpret_cast<size_t>(payload);
    using namespace entt::literals;

    // Retrieve context from Level Registry
    // "get_world_registry" must be stored in m_level_state.ctx()
    auto *get_reg_func = level_registry.ctx().find<std::function<entt::registry &(size_t)>>();

    if (!get_reg_func) {
      ASSERT_FAIL("Fatal: get_world_registry not found in level context!");
      return;
    }

    entt::registry &world_reg = (*get_reg_func)(world_id);

    runtime_context ctx;
    ctx.lvl_registry = &level_registry;
    ctx.current_registry = &world_reg;
    ctx.current_world_id = world_id;
    ctx.get_world_registry = *get_reg_func;

    // Invoke!
    Invoker<Candidate>::invoke(ctx);
  }

  // Registration Dispatcher
  template <auto Candidate> struct RegistrationDispatcher {
    static void dispatch(size_t id, const char *name, entt::organizer &org) {
      switch (id) {
      case 0:
        impl<0>(name, org);
        break;
      case 1:
        impl<1>(name, org);
        break;
      case 2:
        impl<2>(name, org);
        break;
      case 3:
        impl<3>(name, org);
        break;
      case 4:
        impl<4>(name, org);
        break;
      default:
        ASSERT_MSG(false, "World ID > 4 not supported by automatic dispatcher "
                          "(expand switch case if needed)");
      }
    }

    template <size_t ID>
    static void impl(const char *name, entt::organizer &org) {
      using Traits = function_traits<decltype(Candidate)>;
      using DepList = typename args_to_dependencies<typename Traits::args_tuple, ID>::type;

      emplace_automatic_system<Candidate, ID>(name, org, DepList{});
    }
  };

  // --- Public API ---

  // Register Stateful System (Class)
  template <typename T, typename... ARGS>
  void register_system_class(const std::string &name, ARGS... args) {
    ASSERT_MSG(!m_systems.contains(name),
               "ecs system already contains in the factory!");
    m_systems[name] = [name = name, args...](entt::registry &reg,
                                             entt::organizer &org) {
      auto sys = std::make_unique<T>(args...);
      sys->register_in_world(reg, org);
      return sys;
    };
  }

  // Register Automatic System (Function)
  template <auto Candidate>
  void register_automatic_system(const std::string &name) {
    ASSERT_MSG(!m_systems.contains(name), "ecs system already contains in the factory!");

    m_systems[name] = [name](entt::registry &world_reg, entt::organizer &org) {
      using namespace entt::literals;
      size_t w_id = world_reg.ctx().get<ecs::world_index>();

      RegistrationDispatcher<Candidate>::dispatch(w_id, name.c_str(), org);

      return nullptr; // Stateless
    };
  }

  std::unique_ptr<system_interface> create_system(const std::string &name,
                                                  entt::registry &reg,
                                                  entt::organizer &org) {
    if (m_systems.contains(name)) {
      return m_systems[name](reg, org);
    }
    ASSERT_MSG(false, "System not found");
    return nullptr;
  }

  system_factory() = default;

private:
  std::unordered_map<std::string, SystemInitializer> m_systems;
};
} // namespace ecs