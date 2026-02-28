#include "scn_world_desc.h"

void scn::world_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains("name")) {
		name = json::value_to<std::string>(data.at("name"));
	}

	if (data.contains("systems")) {
		systems = json::value_to<std::vector<std::string>>(data.at("systems"));
	}

	scn::prefab_desc::deserialize(desc_system, data);
}

void scn::world_desc::serialize(json::object& data) const
{
	scn::prefab_desc::serialize(data);
	data["systems"] = json::value_from(systems);
	data["name"] = json::value_from(name);
}