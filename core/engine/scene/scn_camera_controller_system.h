#pragma once
#include <snk/ecs/ecs_system.h>
#include <entt/fwd.hpp>
#include "snk_api.h"

namespace scn
{
	SNK_API void init_mouse_controller_system(ecs::system_factory& factory);
}