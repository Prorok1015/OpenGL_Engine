#pragma once
#include <entt/entt.hpp>
#include <tuple>
#include <type_traits>

namespace scn {
template <size_t WorldID, typename T> struct bind_res;
}

namespace ecs {
// --- 0. Common Types ---
struct world_index {
  size_t value;
  operator size_t() const { return value; }
};

// --- 1. Basic Function Traits ---
template <typename> struct function_traits;

template <typename R, typename... Args> struct function_traits<R (*)(Args...)> {
  using return_type = R;
  using args_tuple = std::tuple<Args...>;
  static constexpr size_t args_count = sizeof...(Args);

  template <size_t I> using arg_type = std::tuple_element_t<I, args_tuple>;
};

template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...)> : function_traits<R (*)(Args...)> {};

template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) const>
    : function_traits<R (*)(Args...)> {};

// --- 2. View Detection ---
template <typename T> struct is_view : std::false_type {};

// entt::view is an alias for basic_view<...>.
// We match basic_view generic signature.
template <typename... Args>
struct is_view<entt::basic_view<Args...>> : std::true_type {};

template <typename... Args>
struct is_view<const entt::basic_view<Args...> &> : std::true_type {};

template <typename... Args>
struct is_view<entt::basic_view<Args...> &> : std::true_type {};

// --- 3. bind_res Detection ---
template <typename T> struct is_bind_res : std::false_type {};

template <size_t ID, typename T>
struct is_bind_res<scn::bind_res<ID, T>> : std::true_type {};

template <size_t ID, typename T>
struct is_bind_res<const scn::bind_res<ID, T> &> : std::true_type {};

template <size_t ID, typename T>
struct is_bind_res<scn::bind_res<ID, T> &> : std::true_type {};

// --- 4. salted_type Wrapper ---
// Used to trick EnTT organizer to see different types for different worlds
template <typename T, size_t WorldID> struct salted_type {};

// --- 5. Metaprogramming Helpers for Dependency Extraction ---
template <typename... Ts> struct type_list {};

template <typename... Lists> struct type_list_cat;

template <typename... As, typename... Bs, typename... Rest>
struct type_list_cat<type_list<As...>, type_list<Bs...>, Rest...> {
  using type = typename type_list_cat<type_list<As..., Bs...>, Rest...>::type;
};

template <typename... As> struct type_list_cat<type_list<As...>> {
  using type = type_list<As...>;
};

// Dependency Extractor
template <typename Arg, size_t ContextWorldID> struct dependency_extractor {
  using type = type_list<>; // Default: no dependency
};

// Case: entt::view (Local) -> Extract components inside view
template <typename... GetTypes, typename... ExcludeTypes, typename... Other, size_t ContextWorldID>
struct dependency_extractor<entt::basic_view<entt::get_t<GetTypes...>, entt::exclude_t<ExcludeTypes...>, Other...>, ContextWorldID> {

  // Helper to extract component type from storage type and preserve constness
  template <typename Storage>
  using to_component_type = std::conditional_t<std::is_const_v<Storage>, const typename Storage::value_type, typename Storage::value_type>;

  template <typename T>
  using to_salted = std::conditional_t<std::is_const_v<T>, const salted_type<std::remove_const_t<T>, ContextWorldID>, salted_type<T, ContextWorldID>>;

  using type = type_list<to_salted<to_component_type<GetTypes>>...>;
};

// Support const ref view
template <typename View, size_t ContextWorldID>
struct dependency_extractor<const View &, ContextWorldID>: dependency_extractor<View, ContextWorldID> {};

template <typename View, size_t ContextWorldID>
struct dependency_extractor<View &, ContextWorldID>: dependency_extractor<View, ContextWorldID> {};

// Case: bind_res<TargetID, T> -> salted_type<T, TargetID>
template <size_t TargetID, typename T, size_t ContextWorldID>
struct dependency_extractor<scn::bind_res<TargetID, T>, ContextWorldID> {
  using type = type_list<std::conditional_t<std::is_const_v<T>, const salted_type<std::remove_const_t<T>, TargetID>, salted_type<T, TargetID>>>;
};

template <size_t ID, typename T, size_t CtxID>
struct dependency_extractor<const scn::bind_res<ID, T> &, CtxID>
    : dependency_extractor<scn::bind_res<ID, T>, CtxID> {};

template <size_t ID, typename T, size_t CtxID>
struct dependency_extractor<const scn::bind_res<ID, T>, CtxID>
    : dependency_extractor<scn::bind_res<ID, T>, CtxID> {};

template <size_t ID, typename T, size_t CtxID>
struct dependency_extractor<scn::bind_res<ID, T> &, CtxID>
    : dependency_extractor<scn::bind_res<ID, T>, CtxID> {};

// --- 6. Argument Tuple to Dependency List ---
template <typename Tuple, size_t ContextWorldID> struct args_to_dependencies;

template <typename... Args, size_t ContextWorldID>
struct args_to_dependencies<std::tuple<Args...>, ContextWorldID> {
  using type = typename type_list_cat<typename dependency_extractor<Args, ContextWorldID>::type...>::type;
};

} // namespace ecs
