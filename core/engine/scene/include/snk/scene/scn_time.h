#pragma once
#include <snk/ecs/ecs_invoker.hpp>

namespace scn {

	// Per-context time resources. Each world_context writes them into its own registry ctx every
	// frame; a system takes them as `const scn::delta_time&`.
	struct delta_time { float dt; };
	struct fixed_time { float dt; };
	struct update_alpha { float dt; };

} // namespace scn

namespace ecs {
	// A system asking for one of these must find it in its own context: a missing one is a bug, not a zero.
	template <> inline constexpr bool required_ctx_resource<scn::delta_time> = true;
	template <> inline constexpr bool required_ctx_resource<scn::fixed_time> = true;
	template <> inline constexpr bool required_ctx_resource<scn::update_alpha> = true;
}
