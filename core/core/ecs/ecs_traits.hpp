#pragma once
#include <entt/entt.hpp>
#include <tuple>
#include <type_traits>

namespace ecs {
    template <size_t WorldID, typename T> struct bind_res;

    struct world_salt {
        size_t value;
        operator size_t() const { return value; }
    };

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

    template <typename T> struct is_bind_res : std::false_type {};

    template <size_t ID, typename T>
    struct is_bind_res<ecs::bind_res<ID, T>> : std::true_type {};

    template <size_t ID, typename T>
    struct is_bind_res<const ecs::bind_res<ID, T> &> : std::true_type {};

    template <size_t ID, typename T>
    struct is_bind_res<ecs::bind_res<ID, T> &> : std::true_type {};

    template<typename T>
    inline constexpr bool is_bind_res_v = is_bind_res<T>::value;

    template <typename T, size_t SALT> struct salted_type {};

    template <typename... Ts> struct type_list {};

    template <typename... Lists> struct type_list_cat;

    template <typename... As, typename... Bs, typename... Rest>
    struct type_list_cat<type_list<As...>, type_list<Bs...>, Rest...> {
      using type = typename type_list_cat<type_list<As..., Bs...>, Rest...>::type;
    };

    template <typename... As> struct type_list_cat<type_list<As...>> {
      using type = type_list<As...>;
    };

    template <typename T, size_t SALT> struct dependency_extractor {
      using type = type_list<salted_type<T, SALT>>; // Default: no dependency
    };

    // Case: entt::view (Local) -> Extract components inside view
    template <typename... GET_TYPES, typename... EXCLUDE_TYPES, typename... Other, size_t SALT>
    struct dependency_extractor<entt::basic_view<entt::get_t<GET_TYPES...>, entt::exclude_t<EXCLUDE_TYPES...>, Other...>, SALT> {
        template <typename STORAGE>
        using to_component_type = std::conditional_t<std::is_const_v<STORAGE>, const typename STORAGE::value_type, typename STORAGE::value_type>;

        template <typename T>
        using to_salted = std::conditional_t<std::is_const_v<T>, const salted_type<std::remove_const_t<T>, SALT>, salted_type<T, SALT>>;

        using type = type_list<to_salted<to_component_type<GET_TYPES>>...>;
    };

    // Support const ref view
    template <typename T, size_t SALT>
    struct dependency_extractor<const T&, SALT> : dependency_extractor<T, SALT> {};

    template <typename T, size_t SALT>
    struct dependency_extractor<T&, SALT> : dependency_extractor<T, SALT> {};

    // Case: bind_res<TargetID, T> -> salted_type<T, TargetID>
    template <size_t TargetID, typename T, size_t SALT>
    struct dependency_extractor<ecs::bind_res<TargetID, T>, SALT> {
        using type = type_list<std::conditional_t<std::is_const_v<T>, const salted_type<std::remove_const_t<T>, TargetID>, salted_type<T, TargetID>>>;
    };

    template <size_t ID, typename T, size_t SALT>
    struct dependency_extractor<const ecs::bind_res<ID, T>&, SALT> : dependency_extractor<ecs::bind_res<ID, T>, SALT> {};

    template <size_t ID, typename T, size_t SALT>
    struct dependency_extractor<const ecs::bind_res<ID, T>, SALT> : dependency_extractor<ecs::bind_res<ID, T>, SALT> {};

    template <size_t ID, typename T, size_t SALT>
    struct dependency_extractor<ecs::bind_res<ID, T>&, SALT> : dependency_extractor<ecs::bind_res<ID, T>, SALT> {};

    template <typename Tuple, size_t SALT> struct args_to_dependencies;

    template <typename... ARGS, size_t SALT>
    struct args_to_dependencies<std::tuple<ARGS...>, SALT> {
        using type = typename type_list_cat<typename dependency_extractor<ARGS, SALT>::type...>::type;
    };

} // namespace ecs
