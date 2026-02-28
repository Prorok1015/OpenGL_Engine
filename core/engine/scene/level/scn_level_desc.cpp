#include "scn_level_desc.h"
#include "desc_system.h"

void scn::level_desc::deserialize(desc::desc_system& desc_system, const json::object& obj)
{
	if (obj.contains("worlds")) {
		worlds.clear();
		for (const auto& world_val : obj.at("worlds").as_array()) {
			worlds.push_back(desc_system.get_field_desc<scn::world_desc>(*this, world_val));
		}
	}
}

void scn::level_desc::serialize(json::object& obj) const
{
	json::array worlds_obj;
	for (const auto& world : worlds) {
		json::object world_obj;
		world->serialize(world_obj);
		worlds_obj.push_back(world_obj);
	}
	obj["worlds"] = worlds_obj;
}
