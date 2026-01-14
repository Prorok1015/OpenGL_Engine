#pragma once
#include "ecs_traits.hpp"
#include <entt/entt.hpp>
#include <functional>

namespace scn {
template <size_t ID, typename T> struct bind_res {
  T& ref;

  bind_res(T& r) : ref(r) {}

  T* operator->() const { return &ref; }
  T& operator*() const { return ref; }
  T& get() const { return ref; }
};

} // namespace scn

namespace ecs {
// Context passed to the invoker at runtime
struct runtime_context {
  entt::registry* lvl_registry = nullptr;
  entt::registry* current_registry = nullptr;
  size_t current_world_id = 0;

  // Callback to get any world's registry by ID
  std::function<entt::registry &(size_t)> get_world_registry;
};

// Argument Resolver Template
// Resolves arguments for the System Function from the runtime_context
template <typename T> struct binded_resolver {
  // Fallback: Try to get T from local context
  static decltype(auto) resolve(const runtime_context &ctx) {
      static_assert(false, "this type is not equal bind_res");
  }
};

template <size_t ID, typename... Args> 
struct binded_resolver<const scn::bind_res<ID, entt::basic_view<Args...>>&> {
  static decltype(auto) resolve(const runtime_context &ctx) {
      entt::registry& reg = ctx.get_world_registry(ID);
      return scn::bind_res<ID, entt::basic_view<Args...>>(static_cast<entt::basic_view<Args...>>(entt::as_view{ reg }));
  }
};

template <size_t ID, typename... Args>
struct binded_resolver<scn::bind_res<ID, entt::basic_view<Args...>>> {
  static auto resolve(const runtime_context &ctx) {
    entt::registry& reg = ctx.get_world_registry(ID);
    return scn::bind_res<ID, entt::basic_view<Args...>>(static_cast<entt::basic_view<Args...>>(entt::as_view{ reg }));
  }
};

template <size_t ID, typename T> 
struct binded_resolver<scn::bind_res<ID, T>> {
  static auto resolve(const runtime_context &ctx) {
    entt::registry& r = ctx.get_world_registry(ID);
    T& val = r.ctx().get<std::remove_const_t<T>>();
    return scn::bind_res<ID, T>(val);
  }
};

template <size_t ID, typename T>
struct binded_resolver<const scn::bind_res<ID, T>>
    : binded_resolver<scn::bind_res<ID, T>> {};

template <size_t ID, typename T>
struct binded_resolver<const scn::bind_res<ID, T> &> {
  static auto resolve(const runtime_context &ctx) {
    entt::registry &r = ctx.get_world_registry(ID);
    T& val = r.ctx().get<std::remove_const_t<T>>();
    return scn::bind_res<ID, T>(val);
  }
};

template<typename Type>
inline constexpr bool is_view_v = is_view<Type>::value;

template<typename Type>
inline constexpr bool is_bind_res_v = is_bind_res<Type>::value;

template<typename Type>
[[nodiscard]] static decltype(auto) extract(const runtime_context& ctx) {
    auto& reg = *ctx.current_registry;
    if constexpr (std::is_same_v<Type, entt::registry>) {
        return reg;
    } else if constexpr (is_view_v<Type>) {
        return static_cast<Type>(entt::as_view{ reg });
    } else if constexpr (is_bind_res_v<Type>) {
        return binded_resolver<Type>::resolve(ctx);   
    } else {
        return reg.ctx().template emplace<std::remove_reference_t<Type>>();
    }
}

template<typename... Args>
struct to_args
{
    [[nodiscard]] static auto call(const runtime_context& ctx) {
        return std::tuple<decltype(extract<Args>(ctx))...>(extract<Args>(ctx)...);
    }
};


// --- The Invoker ---
template <auto Candidate> struct Invoker {
  using Traits = function_traits<decltype(Candidate)>;

  static void invoke(const runtime_context &ctx) {
    call(ctx, std::make_index_sequence<Traits::args_count>{});
  }

  template <size_t... Is>
  static void call(const runtime_context &ctx, std::index_sequence<Is...>) {
    // std::invoke handles temporary return values binding to arguments
    std::apply(Candidate, ecs::to_args<typename Traits::template arg_type<Is>...>::call(ctx));
  }
};
} // namespace ecs
