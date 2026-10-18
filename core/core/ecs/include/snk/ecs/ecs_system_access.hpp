#pragma once
#include <snk/ecs/ecs_traits.hpp>
#include <entt/entt.hpp>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

// What a system reads and writes, deduced from its argument types. It is used to *validate* the
// explicit order (a debug warning for two unordered writers), never to derive the order itself.
//  - view<get_t<T>>            : component T, write unless T is const
//  - X& / const X&             : ctx resource or event X in the system's own world, write unless const
//  - bind_res<ID, X>           : resource X of world ID (view -> its components), write unless X is const
//  - entt::registry&           : not tracked (the system reaches components by hand)
namespace ecs {

// World of the system itself; bind_res targets carry their explicit id.
inline constexpr size_t own_world = static_cast<size_t>(-2);

struct access_entry
{
	entt::id_type type = 0;
	std::string_view name;
	size_t world = own_world;
	bool write = false;
};

namespace detail {

template <typename T>
void add_access(std::vector<access_entry>& out, size_t world, bool write)
{
	using bare = std::remove_cv_t<T>;
	out.push_back(access_entry{ entt::type_hash<bare>::value(), entt::type_name<bare>::value(), world, write });
}

template <typename View> struct view_access;

template <typename... GET_TYPES, typename EXCLUDE, typename... Other>
struct view_access<entt::basic_view<entt::get_t<GET_TYPES...>, EXCLUDE, Other...>>
{
	static void collect(std::vector<access_entry>& out, size_t world)
	{
		(add_access<typename std::remove_const_t<GET_TYPES>::value_type>(out, world, !std::is_const_v<GET_TYPES>), ...);
	}
};

template <typename T> struct bind_access;

template <size_t ID, typename T>
struct bind_access<ecs::bind_res<ID, T>>
{
	static void collect(std::vector<access_entry>& out)
	{
		if constexpr (is_view_v<std::remove_cv_t<T>>) {
			view_access<std::remove_cv_t<T>>::collect(out, ID);
		} else {
			add_access<T>(out, ID, !std::is_const_v<T>);
		}
	}
};

} // namespace detail

template <typename ARG>
void collect_argument_access(std::vector<access_entry>& out)
{
	using bare = std::remove_cvref_t<ARG>;
	constexpr bool mutable_ref = std::is_lvalue_reference_v<ARG> && !std::is_const_v<std::remove_reference_t<ARG>>;

	if constexpr (std::is_same_v<bare, entt::registry>) {
		// untracked
	} else if constexpr (is_view_v<bare>) {
		detail::view_access<bare>::collect(out, own_world);
	} else if constexpr (is_bind_res_v<bare>) {
		detail::bind_access<bare>::collect(out);
	} else {
		detail::add_access<bare>(out, own_world, mutable_ref);
	}
}

template <typename... ARGS>
std::vector<access_entry> collect_access(std::tuple<ARGS...>*)
{
	std::vector<access_entry> out;
	(collect_argument_access<ARGS>(out), ...);
	return out;
}

} // namespace ecs
