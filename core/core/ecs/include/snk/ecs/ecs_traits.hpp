#pragma once
#include <snk/ecs/ecs_entt_abi.h>
#include <entt/entt.hpp>
#include <tuple>
#include <type_traits>

namespace ecs {
    template <typename> struct function_traits;

    template <typename R, typename... ARGS> struct function_traits<R (*)(ARGS...)> {
        using return_type = R;
        using args_tuple = std::tuple<ARGS...>;
        static constexpr size_t args_count = sizeof...(ARGS);

        template <size_t I> using arg_type = std::tuple_element_t<I, args_tuple>;
    };

    template <typename C, typename R, typename... ARGS>
    struct function_traits<R (C::*)(ARGS...)> : function_traits<R (*)(ARGS...)> {};

    template <typename C, typename R, typename... ARGS>
    struct function_traits<R (C::*)(ARGS...) const> : function_traits<R (*)(ARGS...)> {};

    template <typename T> struct is_view : std::false_type {};

    // entt::view is an alias for basic_view<...>.
    // We match basic_view generic signature.
    template <typename... ARGS>
    struct is_view<entt::basic_view<ARGS...>> : std::true_type {};

    template <typename... ARGS>
    struct is_view<const entt::basic_view<ARGS...> &> : std::true_type {};

    template <typename... ARGS>
    struct is_view<entt::basic_view<ARGS...> &> : std::true_type {};

    template<typename T>
    inline constexpr bool is_view_v = is_view<T>::value;

} // namespace ecs
