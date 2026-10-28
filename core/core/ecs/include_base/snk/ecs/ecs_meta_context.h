#pragma once
#include <snk/ecs/ecs_component.h>
#include <entt/locator/locator.hpp>
#include <entt/meta/context.hpp>
#include <snk/common/snk_api.h>

// Base only (include_base/, not installed in the SDK): what writes into the engine's meta context.
namespace ecs
{
    // The engine's meta context, writable. Every binary (the editor, a test) has its own
    // entt::locator<entt::meta_ctx> under hidden visibility or as a DLL, so meta registered or resolved
    // there is invisible to the engine's entt::resolve until the binary attaches to this one (#218):
    //   entt::locator<entt::meta_ctx>::reset(ecs::meta_context_handle());
    [[nodiscard]] SNK_API entt::meta_ctx& mutable_meta_context();
    [[nodiscard]] SNK_API entt::locator<entt::meta_ctx>::node_type meta_context_handle();
}
