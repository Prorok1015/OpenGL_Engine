#include "scn_bone_desc.h"
#include "scn_model.h"
#include "scn_glm_json_convert.h"

void scn::bone_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("offset_matrix"))
		offset_matrix = json::value_to<glm::mat4>(*p);
	if (auto const* p = data.if_contains("index"))
		index = p->to_number<int>();
}

void scn::bone_desc::serialize(json::object& data) const
{
	data["offset_matrix"] = json::value_from(offset_matrix);
	data["index"] = index;
}

void scn::assemble_bone(entt::registry& reg, entt::entity e, const bone_desc& desc, std::string_view name)
{
	reg.emplace_or_replace<scn::bone_component>(e, scn::bone_component{
		.offset = desc.offset_matrix,
		.index = desc.index
	});
}
