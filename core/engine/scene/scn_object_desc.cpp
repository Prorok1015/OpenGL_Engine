#include "scn_object_desc.h"
#include "scn_model.h"

void scn::object_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
}

void scn::object_desc::serialize(json::object& data) const
{
}

void scn::assemble_object(entt::registry& reg, entt::entity e, const object_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::object_component>(e);
}
