#pragma once
#include <cstdint>
#include <entt/fwd.hpp>

namespace edt
{
	// Which registry this is, not where it lives: a stamp the editor keeps in the registry's ctx, taken the first time it is
	// asked for. A registry made later gets another one even at the address of a registry that is gone (a world is destroyed
	// and made again by every rebuild, and the allocator hands the same block back). registry::clear() leaves the ctx
	// alone, but gives every entity back with a new version, so an entity taken before it no longer matches any entity.
	// Main thread only.
	std::uint64_t registry_token(entt::registry& reg);
}
