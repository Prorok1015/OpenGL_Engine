#pragma once
#include <snk/common/snk_api.h>
#include <memory>

namespace inp { class input_router; class action_map; }
namespace scn { class context_manager; }

namespace gs
{
	// Writes `inp::input_state` and `inp::input_focus` into the ctx of every context of every set,
	// as the router resolves them: the context under a binding gets the full state, the rest a
	// neutral one. Runs before the contexts update.
	//
	// Inside the winning target the worlds are layered by the router's `inp::input_stack` of the set
	// (INPUT_STACK.md): every world has a base layer "world" (`world_context::input_layer`, the level's
	// `input_stack` defaults, the regions and keyboard flag of its `inp::input_capture`), laid on top of each
	// other by order, then `desc_index`. A layer hides what it takes from the ones below: clicks and wheel in
	// its regions (a button pressed there keeps the mouse with that layer until every button is released),
	// the keys it names, all the rest when it blocks. Mouse motion is never taken by a region, only by a drag
	// a layer holds or by a layer that blocks: a world below a region still gets the movement.
	// Nothing taken: every world of the target gets the full state.
	SNK_API void route_input(inp::input_router& router, scn::context_manager& contexts);

	// The same, and the game actions (WORLD_BUS.md 4.2): every world gets `inp::input_bound` (fed this frame; frozen when
	// not fed while its set steps) and `inp::action_map_ref`, which holds `actions` only for the first world of its set by
	// `desc_index` that lists inp::update_actions, bound or not (INPUT_STACK.md M6, until #624). Null `actions`: no world
	// reads actions.
	SNK_API void route_input(inp::input_router& router, scn::context_manager& contexts,
		const std::shared_ptr<const inp::action_map>& actions);
}
