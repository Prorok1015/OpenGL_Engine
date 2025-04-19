#include "scn_model_desc.h"
#include "desc_system.h"

void scn::model_desc::deserialize(desc::desc_system& desc_system, const json::object& resource)
{
	auto& gmtry = resource.at("geometry_resource");
	if (gmtry.is_string()) {
		geom_desc = desc_system.get_desc<rnd::geometry_desc>(res::tag{ gmtry.as_string() });
	}
}

void scn::model_desc::serialize(const res::tag& tag, res::resource_system& res_system, json::object&) const
{
}
