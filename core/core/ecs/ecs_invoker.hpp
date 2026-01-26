#pragma once
#include "ecs_traits.hpp"
#include <entt/entt.hpp>
#include <functional>
#include "logger/engine_log.h"

namespace ecs {
template <size_t ID, typename T> 
struct bind_res {
    T& ref;

    bind_res(T& r) : ref(r) {}

    T* operator->() const { return &ref; }
    T& operator*() const { return ref; }
    T& get() const { return ref; }
};

// Context passed to the invoker at runtime
struct runtime_context {
    // Callback to get any world's registry by ID
    std::function<entt::registry &(size_t)> get_world_registry;
    entt::registry& current_registry;
    size_t current_world_id = 0;
};

struct runtime_context_provider
{
    using getter_type = std::function<runtime_context(size_t)>;
    runtime_context_provider(getter_type&& get)
        : get_context(get) {
    }
    ~runtime_context_provider() = default;
    runtime_context_provider(const runtime_context_provider&) = default;
    runtime_context_provider(runtime_context_provider&&) = default;
    runtime_context_provider& operator=(const runtime_context_provider&) = default;
    runtime_context_provider& operator=(runtime_context_provider&&) = default;

    getter_type get_context;
};

// Argument Resolver Template
template <typename T> struct binded_resolver {
  // Fallback: Try to get T from local context
  static decltype(auto) resolve(const runtime_context &ctx) {
      static_assert(false, "this type is not equal bind_res");
  }
};

template <size_t ID, typename... ARGS> 
struct binded_resolver<const ecs::bind_res<ID, entt::basic_view<ARGS...>>&> {
  static decltype(auto) resolve(const runtime_context &ctx) {
      entt::registry& reg = ctx.get_world_registry(ID);
      return ecs::bind_res<ID, entt::basic_view<ARGS...>>(static_cast<entt::basic_view<ARGS...>>(entt::as_view{ reg }));
  }
};

template <size_t ID, typename... ARGS>
struct binded_resolver<ecs::bind_res<ID, entt::basic_view<ARGS...>>> {
  static auto resolve(const runtime_context &ctx) {
    entt::registry& reg = ctx.get_world_registry(ID);
    return ecs::bind_res<ID, entt::basic_view<ARGS...>>(static_cast<entt::basic_view<ARGS...>>(entt::as_view{ reg }));
  }
};

template <size_t ID, typename T> 
struct binded_resolver<ecs::bind_res<ID, T>> {
  static auto resolve(const runtime_context &ctx) {
    entt::registry& r = ctx.get_world_registry(ID);
    T& val = r.ctx().get<std::remove_const_t<T>>();
    return ecs::bind_res<ID, T>(val);
  }
};

template <size_t ID, typename T>
struct binded_resolver<const ecs::bind_res<ID, T>> : binded_resolver<ecs::bind_res<ID, T>> {};

template <size_t ID, typename T>
struct binded_resolver<const ecs::bind_res<ID, T> &> {
    static auto resolve(const runtime_context &ctx) {
        entt::registry &r = ctx.get_world_registry(ID);
        T& val = r.ctx().get<std::remove_const_t<T>>();
        return ecs::bind_res<ID, T>(val);
    }
};

template<typename TYPE>
[[nodiscard]] static decltype(auto) extract(const runtime_context& ctx) {
    auto& reg = ctx.current_registry;
    if constexpr (std::is_same_v<std::decay_t<TYPE>, entt::registry>) {
        return reg;
    } else if constexpr (is_view_v<std::decay_t<TYPE>>) {
        return static_cast<TYPE>(entt::as_view{ reg });
    } else if constexpr (is_bind_res_v<std::decay_t<TYPE>>) {
        return binded_resolver<TYPE>::resolve(ctx);   
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
