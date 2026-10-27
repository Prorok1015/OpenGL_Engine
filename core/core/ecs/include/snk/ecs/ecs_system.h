#pragma once
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_invoker.hpp>
#include <snk/ecs/ecs_system_access.hpp>
#include <snk/ecs/ecs_system_interface.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/ecs/ecs_traits.hpp>

#include <entt/entt.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace ecs {

// Registration of systems. The author declares the phase, the ordering constraints and the
// edit-mode flag in code; which systems a world runs is data (world_desc.systems). The scheduler
// (ecs_system_plan.hpp) turns both into one execution plan per world.
//   sf.register_system<&fn>("scn::transform_system",
//       { .phase = ecs::phase::transform, .after = { "scn::depth_system" }, .run_in_edit = true });
class system_factory {
public:
    // Each register_system returns false (and asserts) when the name is already registered.

    // Stateless system (free function).
    template <auto Candidate>
    bool register_system(const std::string &name, system_desc desc = {}) {
        return add_entry(name, std::move(desc), &run_stateless<Candidate>, [] { return system_instance{}; }, access_of<Candidate>());
    }

    // Stateful system shared by every world that enables it.
    template <auto Candidate, typename T>
    requires std::is_base_of_v<ecs::system_interface, T>
    bool register_system(const std::string &name, system_desc desc, std::shared_ptr<T> system) {
        return add_entry(name, std::move(desc), &run_stateful<Candidate, T>,
                  [system] { return system_instance{ system, system.get() }; }, access_of<Candidate>());
    }

    // Stateful system constructed in place, one instance per world that enables it.
    // Reload contract: the instance lives in the world's plan, so every plan rebuild (a hot reload
    // of the systems, reload_world, a phase table change) constructs it anew from ARGS... and its
    // runtime state starts over; that is how a changed desc parameter reaches it. A shared
    // instance (the shared_ptr overload above) is kept across rebuilds and keeps its state.
    template <auto Candidate, typename T, class... ARGS>
    requires std::is_base_of_v<ecs::system_interface, T>
    bool register_system(const std::string &name, system_desc desc, ARGS... args) {
        return add_entry(name, std::move(desc), &run_stateful<Candidate, T>,
                  [args...] { auto system = std::make_shared<T>(args...); return system_instance{ system, system.get() }; },
                  access_of<Candidate>());
    }

    // Adds a named game phase anchored next to a built-in one; false when the name is rejected.
    // Register game phases before the first level load. Plans built earlier do not know the new
    // phase (its systems are dropped as unknown_phase); call scn::context_set::rebuild_all_plans then.
    bool add_game_phase(std::string name, phase anchor, phase_side side) {
        return m_registry.phases().add_game_phase(std::move(name), anchor, side);
    }

    // False when the name is not a game phase. See phase_table::remove_game_phase.
    bool remove_game_phase(std::string_view name) {
        return m_registry.phases().remove_game_phase(name);
    }

    // False when no system has this name. See system_registry::remove for when removing is safe.
    bool unregister_system(const std::string& name) {
        return m_registry.remove(name);
    }

    system_registry& registry() { return m_registry; }
    const system_registry& registry() const { return m_registry; }

    system_factory() = default;
    ~system_factory() = default;
    system_factory(const system_factory&) = default;
    system_factory(system_factory&&) = default;
    system_factory& operator=(const system_factory&) = default;
    system_factory& operator=(system_factory&&) = default;

private:
    bool add_entry(const std::string &name, system_desc desc, system_run_fn run,
                   std::function<system_instance()> make, std::vector<access_entry> access) {
        const bool added = m_registry.add(system_entry{ name, std::move(desc), run, std::move(make), std::move(access) });
        ASSERT_MSG(added, "ecs system already registered in the factory!");
        return added;
    }

    template <auto Candidate>
    static std::vector<access_entry> access_of() {
        using args = typename function_traits<decltype(Candidate)>::args_tuple;
        return collect_access(static_cast<args*>(nullptr));
    }

    template <auto Candidate>
    static void run_stateless(void*, const runtime_context &ctx) {
        invoker<Candidate>::invoke(ctx);
    }

    template <auto Candidate, class T>
    static void run_stateful(void *self, const runtime_context &ctx) {
        invoker<Candidate>::invoke(*static_cast<T*>(self), ctx);
    }

    system_registry m_registry;
};
} // namespace ecs
