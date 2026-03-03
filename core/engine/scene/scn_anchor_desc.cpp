#include "scn_anchor_desc.h"
#include "scn_glm_json_convert.h"
#include "scn_model.h"

void scn::scene_anchor_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
}

void scn::scene_anchor_desc::serialize(json::object& data) const
{
}

void scn::assemble_scene_anchor(entt::registry& reg, entt::entity e, const scene_anchor_desc& desc, const std::string_view name)
{
	reg.emplace<scn::scene_anchor_component>(e);
}