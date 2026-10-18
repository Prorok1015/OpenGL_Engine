#pragma once
#include <entt/fwd.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/common/snk_api.h>

namespace scn
{
	SNK_API void init_transform_system(ecs::system_factory& factory);
}