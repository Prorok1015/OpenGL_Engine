#pragma once
#include <snk/ecs/ecs_entt_abi.h>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <entt/meta/meta.hpp>
#include <entt/meta/factory.hpp>
#include <entt/meta/context.hpp>
#include <entt/meta/resolve.hpp>
#include <entt/locator/locator.hpp>
#include <snk/common/snk_api.h>

namespace ecs
{
    using namespace entt::literals;
	struct input_changed_event_component
	{
	};

    // The component's sandbox_emplace_fn (a function returning it): what world_context::apply_commands moves the
    // component's sandbox values into the world with. The loop, the entity map and the remap of entity fields are the
    // engine's (#784), so only this adapter is compiled in a module.
    inline constexpr entt::hashed_string loader_get_h = "loader_get"_hs;

    // Places a copy of `value` (null for an empty type) on `owner` in the world's storage `id`; the placed component,
    // or null for an empty type.
    using sandbox_emplace_fn = void* (*)(entt::registry& world, entt::id_type id, entt::entity owner, const void* value);

    [[nodiscard]] constexpr auto to_main(ecs::sandbox_entity e) noexcept {
        return static_cast<entt::entity>(static_cast<std::uint32_t>(e));
    }

    template<typename T>
    void* sandbox_emplace(entt::registry& world, entt::id_type id, entt::entity owner, const void* value) {
        auto& storage = world.storage<T>(id);
        // What basic_continuous_loader::get tells a storage without values by.
        if constexpr (std::tuple_size_v<decltype(storage.get_as_tuple({}))> == 0u) {
            storage.emplace(owner);
            return nullptr;
        } else {
            return &storage.emplace(owner, *static_cast<const T*>(value));
        }
    }

    template<typename T>
    sandbox_emplace_fn sandbox_emplace_of() {
        return &sandbox_emplace<T>;
    }

    // The engine's meta context, read-only: a game module registers into its layer's context (scene_layer::meta()).
    // Registering into the engine's one, or attaching a binary's locator to it, is base-only
    // (<snk/ecs/ecs_meta_context.h>, MODULE_LAYER.md).
    [[nodiscard]] SNK_API const entt::meta_ctx& meta_context();

    // `id` in the layer's context first (none: null), then in the engine's.
    [[nodiscard]] inline entt::meta_type find_meta(const entt::meta_ctx* layer, entt::id_type id) {
        if (layer) {
            if (auto type = entt::resolve(*layer, id)) {
                return type;
            }
        }
        return entt::resolve(meta_context(), id);
    }

    template<typename T>
    entt::meta_factory<T> register_component(entt::meta_ctx& ctx) {
        return entt::meta_factory<T>{ ctx }
            .type(entt::type_hash<T>::value())
            .template func<&sandbox_emplace_of<T>>(loader_get_h);
    }
}