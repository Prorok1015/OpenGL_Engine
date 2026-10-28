#pragma once
#include <snk/ecs/ecs_entt_abi.h>
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
//  - entt::registry&           : not tracked (the system reaches components by hand)
namespace ecs {

struct access_entry
{
	entt::id_type type = 0;
	std::string_view name;
	bool write = false;
};

namespace detail {

template <typename T>
void add_access(std::vector<access_entry>& out, bool write)
{
	using bare = std::remove_cv_t<T>;
	out.push_back(access_entry{ entt::type_hash<bare>::value(), entt::type_name<bare>::value(), write });
}

template <typename View> struct view_access;

template <typename... GET_TYPES, typename EXCLUDE, typename... Other>
struct view_access<entt::basic_view<entt::get_t<GET_TYPES...>, EXCLUDE, Other...>>
{
	static void collect(std::vector<access_entry>& out)
	{
		(add_access<typename std::remove_const_t<GET_TYPES>::value_type>(out, !std::is_const_v<GET_TYPES>), ...);
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
		detail::view_access<bare>::collect(out);
	} else {
		detail::add_access<bare>(out, mutable_ref);
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
