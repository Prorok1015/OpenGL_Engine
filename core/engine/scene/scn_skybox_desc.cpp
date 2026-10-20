#include <snk/scene/scn_skybox_desc.h>
#include <snk/scene/scn_model.h>

void scn::assemble_skybox(entt::registry& reg, entt::entity e, const skybox_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::sky_component>(e);
	reg.emplace_or_replace<scn::renderable>(e);
	reg.emplace_or_replace<scn::material_desc_component>(e, scn::material_desc_component{ desc.material });
}