#include <snk/game/gs_input_routing.h>
#include <snk/input/inp_input_router.h>
#include <snk/input/inp_events.hpp>
#include <snk/scene/level/scn_context_manager.h>

void gs::route_input(const inp::input_router& router, scn::context_manager& contexts)
{
	contexts.for_each_set([&router](scn::context_set& set) {
		set.for_each([&router, &set](scn::world_context& context) {
			const inp::routed_input routed = router.route(set.name(), context.get_name());
			auto& ctx = context.state().ctx();
			ctx.insert_or_assign(routed.state);
			ctx.insert_or_assign(inp::input_focus{ routed.focused });
		});
	});
}
