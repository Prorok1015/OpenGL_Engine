#include <snk/scene/level/scn_world_desc.h>

void scn::world_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains("name")) {
		name = json::value_to<std::string>(data.at("name"));
	}

	if (data.contains("systems")) {
		systems = json::value_to<std::vector<std::string>>(data.at("systems"));
	}

	if (data.contains("input_layer")) {
		input_layer = json::value_to<int>(data.at("input_layer"));
	}

	scn::prefab_desc::deserialize(desc_system, data);
}

void scn::world_desc::serialize(json::object& data) const
{
	json::object node;
	scn::prefab_desc::serialize(node);
	// The world's own fields go first, the node tree after them.
	data.clear();
	data["__type"] = "world_desc";
	data["name"] = json::value_from(name);
	data["systems"] = json::value_from(systems);
	// The default is not written, so levels saved before the field existed stay byte-identical.
	if (input_layer != 0) {
		data["input_layer"] = input_layer;
	}
	for (auto& member : node) {
		data[member.key()] = std::move(member.value());
	}
}