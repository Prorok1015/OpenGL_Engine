#include "scn_animated_node_desc.h"
#include "scn_model.h"

void scn::animated_node_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	// No fields — node_name comes from the prefab node name at assembly time
}

void scn::animated_node_desc::serialize(json::object& data) const
{
	// No fields to serialize
}

void scn::assemble_animated_node(entt::registry& reg, entt::entity e,
                                 const animated_node_desc& desc, std::string_view name)
{
	// name parameter is the component key ("animated_node_desc"), not the node name.
	// Use the entity's name_component which was set during prefab assembly.
	std::string node_name;
	if (auto* nc = reg.try_get<scn::name_component>(e)) {
		node_name = nc->name;
	}
	reg.emplace_or_replace<scn::animated_node_component>(e,
		scn::animated_node_component{ .node_name = std::move(node_name) });
}
