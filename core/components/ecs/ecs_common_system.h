#pragma once
#include <entt/entt.hpp>

namespace ecs
{
	extern entt::registry registry; // TODO: remove this global registry. make registries local to systems that need them.
}
