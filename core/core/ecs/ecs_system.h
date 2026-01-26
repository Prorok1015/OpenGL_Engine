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

#include "ecs_command_buffer.hpp"
#include "ecs_invoker.hpp" // Has invoker, runtime_context
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
  using system_initializer_fn = std::function<std::shared_ptr<system_interface>( entt::registry &, entt::organizer &)>;

  static system_factory &instance() {
    static system_factory inst;
    return inst;
  }

  // --- Core Dispatcher Logic for Automatic Systems ---
  template <auto Candidate, size_t WorldID, typename... SaltedReqs>
  static void emplace_automatic_system(const char* name, entt::organizer &org, type_list<SaltedReqs...>) {
    org.emplace<SaltedReqs...>(&InvokerCallback<Candidate, WorldID>, nullptr, name);
  }

  template <auto Candidate, class INSTANCE, size_t WorldID, typename... SaltedReqs>
  static void emplace_automatic_system(INSTANCE* instance, const char* name, entt::organizer &org, type_list<SaltedReqs...>) {
    org.emplace<SaltedReqs...>(&InvokerCallback<Candidate, INSTANCE, WorldID>, instance, name);
  }

  // The generic callback executed by Organizer
  template <auto Candidate, class INSTANCE, size_t world_id>
  static void InvokerCallback(const void *payload, entt::registry &level_registry) {
    auto* get_reg_func = level_registry.ctx().find<runtime_context_provider>();

    if (!get_reg_func) {
      ASSERT_FAIL("Fatal: runtime_context_provider not found in level context!");
      return;
    }

    runtime_context ctx = get_reg_func->get_context(world_id);
    invoker<Candidate>::invoke(*static_cast<INSTANCE*>(const_cast<void*>(payload)), ctx);
  }

  template <auto Candidate, size_t world_id>
  static void InvokerCallback(const void *payload, entt::registry &level_registry) {
    auto* get_reg_func = level_registry.ctx().find<runtime_context_provider>();

    if (!get_reg_func) {
      ASSERT_FAIL("Fatal: runtime_context_provider not found in level context!");
      return;
    }

    runtime_context ctx = get_reg_func->get_context(world_id);
    invoker<Candidate>::invoke(ctx);
  }

  // Registration Dispatcher
  template <auto Candidate, class INSTANCE> struct registration_dispatcher {
    static void dispatch(size_t id, INSTANCE* instance, const char* name, entt::organizer &org) {
      switch (id) {
      case 0:
        impl<0>(instance, name, org);
        break;
      case 1:
        impl<1>(instance, name, org);
        break;
      case 2:
        impl<2>(instance, name, org);
        break;
      case 3:
        impl<3>(instance, name, org);
        break;
      case 4:
        impl<4>(instance, name, org);
        break;
      default:
        ASSERT_MSG(false, "World ID > 4 not supported by automatic dispatcher "
                          "(expand switch case if needed)");
      }
    }

    template <size_t ID>
    static void impl(INSTANCE* instance, const char* name, entt::organizer &org) {
      using Traits = function_traits<decltype(Candidate)>;
      using DepList = typename args_to_dependencies<typename Traits::args_tuple, ID>::type;
      if constexpr (std::is_same_v<INSTANCE, void>)
        emplace_automatic_system<Candidate, ID>(name, org, DepList{});
      else
        emplace_automatic_system<Candidate, INSTANCE, ID>(instance, name, org, DepList{});
    }
  };

  // --- Public API ---

  // Register Stateful System (Class)
    template <auto Candidate, typename T>
    //requires std::is_base_of_v<ecs::system_interface, T>
    void register_automatic_system(const std::string &name, std::shared_ptr<T> system) {
        ASSERT_MSG(!m_systems.contains(name), "ecs system already contains in the factory!");

        m_systems[name] = [name = name, system = system](entt::registry& world_reg, entt::organizer &org) {
            size_t w_id = world_reg.ctx().get<ecs::world_salt>();

            registration_dispatcher<Candidate, T>::dispatch(w_id, system.get(), name.c_str(), org);

            return system;
        };
    }

    template <auto Candidate, typename T, class... ARGS>
    requires std::is_base_of_v<ecs::system_interface, T>
    void register_system_class(const std::string &name, ARGS... args) {
        ASSERT_MSG(!m_systems.contains(name), "ecs system already contains in the factory!");

        m_systems[name] = [name = name, args...](entt::registry& world_reg, entt::organizer& org) {
            size_t w_id = world_reg.ctx().get<ecs::world_salt>();
            auto system = std::make_shared<T>(args...);

            registration_dispatcher<Candidate, T>::dispatch(w_id, system.get(), name.c_str(), org);

            return system;
        };
    }

  // Register Automatic System (Function)
    template <auto Candidate>
    void register_automatic_system(const std::string &name) {
        ASSERT_MSG(!m_systems.contains(name), "ecs system already contains in the factory!");

        m_systems[name] = [name](entt::registry &world_reg, entt::organizer &org) {
            size_t w_id = world_reg.ctx().get<ecs::world_salt>();

            registration_dispatcher<Candidate, void>::dispatch(w_id, nullptr, name.c_str(), org);

            return nullptr; // Stateless
        };
    }

  std::shared_ptr<system_interface> create_system(const std::string &name, entt::registry &reg, entt::organizer &org)  {
    if (m_systems.contains(name)) {
      return m_systems[name](reg, org);
    }

    ASSERT_FAIL("System is not found");
    return nullptr;
  }

  system_factory() = default;
private:
  std::unordered_map<std::string, system_initializer_fn> m_systems;
};
} // namespace ecs