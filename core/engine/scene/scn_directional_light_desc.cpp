#include "scn_directional_light_desc.h"
#include "scn_glm_json_convert.h"

#include "scn_model.h"

void scn::directional_light_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains("direction"))
		direction = json::value_to<glm::vec4>(data.at("direction"));
	if (data.contains("diffuse"))
		diffuse = json::value_to<glm::vec4>(data.at("diffuse"));
	if (data.contains("ambient"))
		ambient = json::value_to<glm::vec4>(data.at("ambient"));
	if (data.contains("specular"))
		specular = json::value_to<glm::vec4>(data.at("specular"));
}

void scn::directional_light_desc::serialize(json::object& data) const
{
	data["direction"] = json::value_from(direction);
	data["diffuse"] = json::value_from(diffuse);
	data["ambient"] = json::value_from(ambient);
	data["specular"] = json::value_from(specular);
}

void scn::assemble_directional_light(entt::registry& reg, entt::entity e, const directional_light_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::renderable>(e);
	reg.emplace_or_replace<scn::directional_light>(e, scn::directional_light{ .direction = desc.direction, .diffuse = desc.diffuse, .ambient = desc.ambient, .specular = desc.specular });
}