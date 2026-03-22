#include "scn_skinning_desc.h"
#include "scn_model.h"
#include "res_tag.h"

void scn::skinning_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("bone_count"))
		bone_count = p->to_number<int>();
	if (auto const* p = data.if_contains("skinning_tag"))
		skinning_tag = json::value_to<res::tag>(*p);
}

void scn::skinning_desc::serialize(json::object& data) const
{
	data["bone_count"] = bone_count;
	if (skinning_tag.is_valid())
		data["skinning_tag"] = json::value_from(skinning_tag);
}

static entt::entity find_skeleton_root(entt::registry& reg, entt::entity mesh_entity)
{
	// Walk up the parent hierarchy to find the prefab root.
	// Stop before scene_anchor_component (world root).
	entt::entity current = mesh_entity;
	while (auto* pc = reg.try_get<scn::parent_component>(current)) {
		if (reg.all_of<scn::scene_anchor_component>(pc->parent)) break;
		current = pc->parent;
	}
	return current;
}

void scn::assemble_skinning(entt::registry& reg, entt::entity e, const skinning_desc& desc, std::string_view name)
{
	entt::entity skeleton_root = find_skeleton_root(reg, e);

	// Ensure skeleton_component exists on root
	if (!reg.all_of<scn::skeleton_component>(skeleton_root)) {
		reg.emplace<scn::skeleton_component>(skeleton_root,
			scn::skeleton_component{ .skinning_tag = desc.skinning_tag, .bone_count = desc.bone_count });
	} else {
		// Update bone_count if this mesh has more bones
		auto& skel = reg.get<scn::skeleton_component>(skeleton_root);
		if (desc.bone_count > skel.bone_count) {
			skel.bone_count = desc.bone_count;
		}
		if (!skel.skinning_tag.is_valid() && desc.skinning_tag.is_valid()) {
			skel.skinning_tag = desc.skinning_tag;
		}
	}

	// Ensure bone_matrices_component exists on skeleton root (not on mesh)
	if (desc.bone_count > 0) {
		if (!reg.all_of<scn::bone_matrices_component>(skeleton_root)) {
			auto& bm = reg.emplace<scn::bone_matrices_component>(skeleton_root);
			bm.matrices.resize(desc.bone_count, glm::mat4{ 1.0f });
		} else {
			auto& bm = reg.get<scn::bone_matrices_component>(skeleton_root);
			if ((int)bm.matrices.size() < desc.bone_count) {
				bm.matrices.resize(desc.bone_count, glm::mat4{ 1.0f });
			}
		}
	}

	// Set skinning_component on the mesh entity with reference to skeleton root
	reg.emplace_or_replace<scn::skinning_component>(e,
		scn::skinning_component{ .skinning_tag = desc.skinning_tag, .skeleton_entity = skeleton_root });
}
