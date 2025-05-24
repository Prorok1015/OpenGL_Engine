#include "scn_skinning_prototype_desc.h"

void scn::skinning_prototype_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	animatable_prototype_desc::deserialize(desc_system, data);
}

void scn::skinning_prototype_desc::serialize(json::object& data) const
{
	animatable_prototype_desc::serialize(data);
}
