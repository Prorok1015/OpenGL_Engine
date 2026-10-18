#pragma once
#include <cstddef>
#include <string_view>
#include <entt/fwd.hpp>
#include "level/scn_level.h"

namespace edt
{
	// In the editor the 3D view is shown inside an ImGui panel, so a camera must not draw into the
	// window's backbuffer (it would be painted over by the UI). Every camera of `reg` that targets
	// the backbuffer is given its own texture target instead. `scope` (the world name) keeps the
	// target tags of different worlds apart. Returns how many cameras were redirected.
	std::size_t redirect_backbuffer_cameras(entt::registry& reg, std::string_view scope);

	// Same for every world of the level; run each frame before extraction so cameras created
	// by level loads and hot reloads are covered too.
	void redirect_backbuffer_cameras(scn::level& level);
}
