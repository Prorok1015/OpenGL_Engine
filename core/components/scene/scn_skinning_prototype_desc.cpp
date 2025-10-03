#include "scn_skinning_prototype_desc.h"
#include "scn_model.h"
#include "scn_glm_json_convert.h"

void scn::skinning_prototype_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	curIndex = 0;
	animatable_prototype_desc::deserialize(desc_system, data);
	if (auto* tree = data.if_contains("tree")) {
		if (auto* tree_obj = tree->if_object()) {
			deserialize_bones(*tree_obj);
		}
	}
}

void scn::skinning_prototype_desc::serialize(json::object& data) const
{
	animatable_prototype_desc::serialize(data);

	if (auto* tree = data.if_contains("tree")) {
		if (auto* tree_obj = tree->if_object()) {
			serialize_bones(*tree_obj);
		}
	}
}

void scn::skinning_prototype_desc::load_prototype(entt::registry& registry, entt::entity parent)
{
	load_context ctx;
	auto root_ent = load_prototype_node(registry, parent, root, ctx);
	animatable_prototype_desc::load_prototype_animations(registry, root_ent);
	registry.emplace<skinning_component>(root_ent, get_tag());
	auto& matrs = registry.emplace<bone_matrices_component>(root_ent);
	matrs.matrices.resize(curIndex + 1, glm::mat4{1.0});

	//
	//   bone_matr_offsets -> [bone_name -> offset_matrix] -> [vertex, bone_name -> weight]
	//   1. collect matrix offsets into a array +
	//   2. map bone_name to index in the array +
	//   3. assign the index to the bone_component. we will set calculated matrix in animation job by this index +
	//   4. collect weights and bone id into rangged 2d array 
	//   5. save that array into ssbo
	//   6. save desc into mesh component. we will load ssbo buffer in renderer by skinning manager +
}

std::vector<std::vector<uint32_t>> scn::skinning_prototype_desc::get_2d_array_bonesids_weights() const
{
	std::vector<std::vector<uint32_t>> res;
	for (const auto& mesh_info : bones_weights) {
		for (int idx = 0; idx < mesh_info.vertex_weights.size(); ++idx) {
			const auto& warray = mesh_info.vertex_weights[idx];
			std::vector<uint32_t> column;
			column.resize(warray.size() * 2);
			for(int idw = 0; idw < warray.size(); ++idw)
			{
				const int cur_bidx = idw;
				const int cur_widx = warray.size() + idw;
				const auto& [bname, bweight] = warray[idw];
				if (auto itb = bones_offsets.find(bname); itb != bones_offsets.end()) {
					const auto& [bmatr, bidx] = itb->second;
					column[cur_bidx] = bidx;
					column[cur_widx] = std::bit_cast<uint32_t>(bweight);
				} else {
					egLOG("model/load", "Can't find bone name '{0}' in offsets for mesh '{1}'", bname, mesh_info.mesh_name);
					column[cur_bidx] = 0;
					column[cur_widx] = 0;
				}
			}
			res.push_back(column);
		}
	}
	return res;
}

entt::entity scn::skinning_prototype_desc::load_prototype_node(entt::registry& registry, entt::entity parent, const node_t& node, load_context& ctx) const
{
	auto ent = animatable_prototype_desc::load_prototype_node(registry, parent, node, ctx);

	if (auto it = bones_offsets.find(node.name); it != bones_offsets.end()) {
		auto& [matr, index] = it->second;
		registry.emplace<scn::bone_component>(ent, matr, index);
	}

	if (node.mesh.has_value()) {
		// add skinning desc to mesh component 'ent'
		registry.emplace<skinning_component>(ent, get_tag());
	}

	return ent;
}

void scn::skinning_prototype_desc::deserialize_bones(const json::object& obj)
{
	if (auto* bone = obj.if_contains("bone")) {
		if (auto* obj_bone = bone->if_object()) {
			std::string name = json::value_to<std::string>(obj_bone->at("name"));
			glm::mat4 offset = json::value_to<glm::mat4>(obj_bone->at("offset_matrix"));
			bones_offsets[name] = { offset, curIndex };
			++curIndex;
		}
	}

	if (auto* mesh = obj.if_contains("mesh")) {
		bones_influence bone;
		if (auto* name = obj.if_contains("name")) {
			bone.mesh_name = json::value_to<std::string>(*name);
		}
		if (auto* omesh = mesh->if_object()) {
			if (const auto* weights = omesh->if_contains("weights")) {
				if (auto* arr_weights = weights->if_array()) {
					auto& vertex_weights = bone.vertex_weights;
					vertex_weights.resize(arr_weights->size());
					std::size_t idx = 0;
					for (const auto& w1 : *arr_weights) {
						if (w1.is_null()) continue;

						for (const auto& weight : w1.as_array()) {
							if (weight.is_object()) {
								skinning_prototype_desc::weight w;
								w.bname = json::value_to<std::string>(weight.at("bone_name"));
								w.bweight = json::value_to<float>(weight.at("weight"));
								vertex_weights[idx].push_back(w);
							}
						}
						++idx;
					}

				}
			}
		}
		bones_weights.push_back(bone);
	}

	if (obj.contains("children")) {
		auto children_array = obj.at("children").as_array();
		for (auto& child : children_array) {
			deserialize_bones(child.as_object());
		}
	}
}

void scn::skinning_prototype_desc::serialize_bones(json::object& obj) const
{
	if (auto* name = obj.if_contains("name")) {
		
		if (auto it = bones_offsets.find(json::value_to<std::string>(*name)); 
			it != bones_offsets.end()) 
		{
			obj["bone"] = json::object
			{
				{"name", *name},
				{"offset_matrix", json::value_from(it->second)}
			};
		}
	}

	if (obj.contains("children")) {
		auto children_array = obj.at("children").as_array();
		for (auto& child : children_array) {
			serialize_bones(child.as_object());
		}
	}
}
