#include <snk/scene/scn_directional_light_desc.h>
#include <snk/scene/scn_model.h>

void scn::assemble_directional_light(entt::registry& reg, entt::entity e, const directional_light_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::renderable>(e);
	reg.emplace_or_replace<scn::directional_light>(e, scn::directional_light{ .direction = desc.direction, .diffuse = desc.diffuse, .ambient = desc.ambient, .specular = desc.specular });
}