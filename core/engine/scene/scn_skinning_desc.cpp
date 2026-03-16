#include "scn_skinning_desc.h"
#include "scn_model.h"

void scn::skinning_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("bone_count"))
		bone_count = p->to_number<int>();
}

void scn::skinning_desc::serialize(json::object& data) const
{
	data["bone_count"] = bone_count;
}

void scn::assemble_skinning(entt::registry& reg, entt::entity e, const skinning_desc& desc, std::string_view name)
{
	reg.emplace_or_replace<scn::skinning_component>(e, scn::skinning_component{});

	if (desc.bone_count > 0) {
		auto& bm = reg.emplace_or_replace<scn::bone_matrices_component>(e);
		bm.matrices.resize(desc.bone_count, glm::mat4{ 1.0f });
	}
}
