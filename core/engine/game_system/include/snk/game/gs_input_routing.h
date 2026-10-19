#pragma once
#include <snk/common/snk_api.h>

namespace inp { class input_router; }
namespace scn { class context_manager; }

namespace gs
{
	// Writes `inp::input_state` and `inp::input_focus` into the ctx of every context of every set,
	// as the router resolves them: the context under a binding gets the full state, the rest a
	// neutral one. Runs before the contexts update.
	SNK_API void route_input(const inp::input_router& router, scn::context_manager& contexts);
}
