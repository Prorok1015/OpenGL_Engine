#include <snk/scene/scn_anchor_desc.h>
#include <snk/scene/scn_glm_json_convert.h>
#include <snk/scene/scn_model.h>

void scn::assemble_scene_anchor(entt::registry& reg, entt::entity e, const scene_anchor_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::scene_anchor_component>(e);
}