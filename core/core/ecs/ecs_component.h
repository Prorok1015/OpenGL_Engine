#pragma once
#include "ecs_command_buffer.hpp"
#include <entt/meta/meta.hpp>
#include <entt/meta/factory.hpp>
#include <iostream>
namespace ecs
{
    using namespace entt::literals;
	struct input_changed_event_component
	{
	};

    inline constexpr entt::hashed_string snapshot_get_h = "snapshot_get"_hs;
    inline constexpr entt::hashed_string loader_get_h = "loader_get"_hs;

    [[nodiscard]] constexpr auto to_main(ecs::sandbox_entity e) noexcept {
        return static_cast<entt::entity>(static_cast<std::uint32_t>(e));
    }

    template<typename T>
    void meta_snapshot_get(const sandbox_snapshot& snapshot, meta_archive& archive) {
        snapshot.get<T>(archive);
    }

    template<typename T>
    void meta_loader_get(entt::continuous_loader* loader, const entt::basic_sparse_set<ecs::sandbox_entity>* raw_storage_ptr) {
        const auto* storage_ptr = static_cast<const entt::basic_storage<T, ecs::sandbox_entity>*>(raw_storage_ptr);

        if (!storage_ptr) return;

        ecs::sandbox_loader_archive archive{ *loader, *storage_ptr };

        loader->get<T>(archive);
        std::cout << "get loader call" << std::endl;
    }

    template<typename T>
    decltype(auto) register_component(const char* name) {
        return entt::meta<T>()
            .type(entt::type_hash<T>::value())
            .func<&meta_snapshot_get<T>>(snapshot_get_h)
            .func<&meta_loader_get<T>>(loader_get_h);
    }
}