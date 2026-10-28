#pragma once
#include <snk/ecs/ecs_entt_abi.h>
#include <snk/ecs/ecs_traits.hpp>
#include <entt/entt.hpp>
#include <functional>
#include <snk/common/logger/engine_log.h>
#include <snk/common/engine_assert.h>

namespace ecs {

// Specialize to true for a ctx resource that the context must have written before any system
// runs. Asking for it when it is missing asserts instead of creating a zeroed one.
template <class T>
inline constexpr bool required_ctx_resource = false;

// Context passed to the invoker at runtime
struct runtime_context {
    entt::registry& current_registry;
    size_t current_world_id = 0;
};

template<typename TYPE>
[[nodiscard]] static decltype(auto) extract(const runtime_context& ctx) {
    auto& reg = ctx.current_registry;
    if constexpr (std::is_same_v<std::decay_t<TYPE>, entt::registry>) {
        return reg;
    } else if constexpr (is_view_v<std::decay_t<TYPE>>) {
        return static_cast<TYPE>(entt::as_view{ reg });
    } else if constexpr (required_ctx_resource<std::remove_cvref_t<TYPE>>) {
        using res_t = std::remove_cvref_t<TYPE>;
        res_t* res = reg.ctx().template find<res_t>();
        if (!res) {
            ASSERT_FAIL("system requests ctx resource '{}' that its context did not write", entt::type_name<res_t>::value());
            res = &reg.ctx().template emplace<res_t>(); // release builds only
        }
        return *res;
    } else {
        return reg.ctx().template emplace<std::remove_reference_t<TYPE>>();
    }
}

template<typename... ARGS>
struct to_args
{
    [[nodiscard]] static auto call(const runtime_context& ctx) {
        return std::tuple<decltype(extract<ARGS>(ctx))...>(extract<ARGS>(ctx)...);
    }
};

template<auto Candidate> struct invoker {
    using Traits = function_traits<decltype(Candidate)>;

    static void invoke(const runtime_context &ctx) {
        call(ctx, std::make_index_sequence<Traits::args_count>{});
    }

    template<class INSTANCE>
    static void invoke(INSTANCE& value_or_instance, const runtime_context &ctx) {
        call<INSTANCE>(value_or_instance, ctx, std::make_index_sequence<Traits::args_count>{});
    }

private:
    template <size_t... TYPE_INDEX>
    static void call(const runtime_context &ctx, std::index_sequence<TYPE_INDEX...>) {
        std::apply(Candidate, ecs::to_args<typename Traits::template arg_type<TYPE_INDEX>...>::call(ctx));
    }

    template <class INSTANCE, size_t... TYPE_INDEX>
    static void call(INSTANCE& value_or_instance, const runtime_context &ctx, std::index_sequence<TYPE_INDEX...>) {
        std::apply(Candidate, std::tuple_cat(std::forward_as_tuple(value_or_instance), ecs::to_args<typename Traits::template arg_type<TYPE_INDEX>...>::call(ctx)));
    }
};
} // namespace ecs
