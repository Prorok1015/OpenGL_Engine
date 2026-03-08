#include "scn_skybox_desc.h"
#include "scn_model.h"
#include "desc_system.h"

void scn::skybox_desc::deserialize(desc::desc_system& desc_system, const json::object& resource)
{
	if (resource.contains("material")) {
		sky_material_desc = desc_system.get_field_desc<scn::material_desc>(*this, resource.at("material"));
	}
}

void scn::skybox_desc::serialize(json::object& resource) const
{
	if (sky_material_desc.is_valid()) {
		resource["material"] = json::value_from(sky_material_desc->get_tag());
	}
}

void scn::assemble_skybox(entt::registry& reg, entt::entity e, const skybox_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::sky_component>(e);
	reg.emplace_or_replace<scn::renderable>(e);
	reg.emplace_or_replace<scn::material_desc_component>(e, scn::material_desc_component{ desc.sky_material_desc });
}