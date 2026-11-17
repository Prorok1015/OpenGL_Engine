#include <snk/scene/scn_screen_anchor_desc.h>
#include <snk/scene/level/scn_screen_anchor.h>
#include <entt/entt.hpp>

void scn::assemble_screen_anchor(entt::registry& reg, entt::entity e, const screen_anchor_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::screen_anchor_component>(e, scn::screen_anchor_component{
		.target = ecs::entity_ref{ desc.target_context, desc.target_path },
		.offset = desc.offset });
}
