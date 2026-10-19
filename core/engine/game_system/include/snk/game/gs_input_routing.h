#pragma once
#include <snk/common/snk_api.h>

namespace inp { class input_router; }
namespace scn { class context_manager; }

namespace gs
{
	// Writes `inp::input_state` and `inp::input_focus` into the ctx of every context of every set,
	// as the router resolves them: the context under a binding gets the full state, the rest a
	// neutral one. Runs before the contexts update.
	//
	// Inside the winning target the contexts are ordered by `world_context::input_layer` (top
	// first). The first one whose `inp::input_capture` regions contain the cursor of this frame
	// gets the mouse, the others a neutral one; a button pressed there keeps the mouse with that
	// context until every button is released (a drag that started elsewhere never reaches the
	// regions it crosses). The keyboard goes to the top context with `input_capture::keyboard`.
	// No region hit and no keyboard owner: every context of the target gets the full state.
	SNK_API void route_input(inp::input_router& router, scn::context_manager& contexts);
}
