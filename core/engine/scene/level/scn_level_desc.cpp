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
	json::array worlds_arr;
	for (const auto& world : worlds) {
		json::object world_obj;
		world->serialize(world_obj);
		worlds_arr.push_back(std::move(world_obj));
	}
	obj["worlds"] = std::move(worlds_arr);
}

json::object scn::level_desc::build_json(std::string_view name, const std::vector<world_desc>& worlds)
{
	json::array worlds_arr;
	for (const auto& wd : worlds) {
		json::object w;
		wd.serialize(w);
		worlds_arr.push_back(std::move(w));
	}

	json::object obj;
	obj["__type"] = std::string{ scn::level_desc::__type };
	obj["name"]   = std::string{ name };
	obj["worlds"] = std::move(worlds_arr);
	return obj;
}
