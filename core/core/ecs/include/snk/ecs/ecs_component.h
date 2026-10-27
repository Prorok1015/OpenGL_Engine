#pragma once
#include <snk/ecs/ecs_command_buffer.hpp>
#include <entt/meta/meta.hpp>
#include <entt/meta/factory.hpp>
#include <entt/meta/context.hpp>
#include <entt/locator/locator.hpp>
#include <snk/common/snk_api.h>
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
    }

    // The engine's meta context. Every binary (the editor, a game module) has its own
    // entt::locator<entt::meta_ctx> under hidden visibility or as a DLL, so meta registered or resolved
    // there is invisible to the engine's entt::resolve until the binary attaches to this one (#218):
    //   entt::locator<entt::meta_ctx>::reset(ecs::meta_context_handle());
    [[nodiscard]] SNK_API entt::meta_ctx& meta_context();
    [[nodiscard]] SNK_API entt::locator<entt::meta_ctx>::node_type meta_context_handle();

    template<typename T>
    entt::meta_factory<T> register_component(entt::meta_ctx& ctx) {
        return entt::meta_factory<T>{ ctx }
            .type(entt::type_hash<T>::value())
            .template func<&meta_snapshot_get<T>>(snapshot_get_h)
            .template func<&meta_loader_get<T>>(loader_get_h);
    }

    template<typename T>
    entt::meta_factory<T> register_component(const char* name) {
        return register_component<T>(entt::locator<entt::meta_ctx>::value_or());
    }
}