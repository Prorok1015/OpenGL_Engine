#pragma once
#include <snk/ecs/ecs_invoker.hpp>

namespace scn {

	// Per-context time resources. Each world_context writes them into its own registry ctx every
	// frame; a system takes them as `const scn::delta_time&`.
	struct delta_time { float dt; };
	struct fixed_time { float dt; };
	struct update_alpha { float dt; };
	// The sum of the delta_time of every frame: it stands in Pause, moves by one fixed step in Step and follows the
	// time scale. Only a level load (reset_time) sets it to 0; a context joining a set takes the time of the set and a
	// reloaded world keeps its own. Shaders get it as `time`.
	struct world_time { double seconds; };

} // namespace scn

namespace ecs {
	// A system asking for one of these must find it in its own context: a missing one is a bug, not a zero.
	template <> inline constexpr bool required_ctx_resource<scn::delta_time> = true;
	template <> inline constexpr bool required_ctx_resource<scn::fixed_time> = true;
	template <> inline constexpr bool required_ctx_resource<scn::update_alpha> = true;
}
