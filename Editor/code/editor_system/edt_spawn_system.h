#pragma once
#include <entt/fwd.hpp>
#include "scn_model.h"
#include "ecs_command_buffer.hpp"

namespace edt {
	void spawn_system(entt::registry& spawner);
}