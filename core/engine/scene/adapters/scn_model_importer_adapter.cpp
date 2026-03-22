#include "scn_model_importer_adapter.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "res_system.h"
#include "eng_profiler.h"

#include "geom/rnd_geometry_desc.h"
#include "adapters/res_pct_adapter.h"

#include "scn_glm_json_convert.h"
#include "scn_mesh_nodes.hpp"

#include "scn_assimp_resource_system_wrapper.h"

#include "ds/ds_svg_writer.hpp"
#include <unordered_set>

void process_model(const aiScene* scene, json::object& data, res::tag tag, res::tag prefab_tag);

namespace {
	glm::quat convert_to_glm(const aiQuaternion& vector) {
		glm::quat result;
		result.x = vector.x;
		result.y = vector.y;
		result.z = vector.z;
		result.w = vector.w;
		return result;
	}

	glm::vec3 convert_to_glm(const aiVector3D& vector) {
		glm::vec3 result;
		result.x = vector.x;
		result.y = vector.y;
		result.z = vector.z;
		return result;
	}

	glm::vec4 convert_to_glm(const aiColor4D& vector) {
		glm::vec4 result;
		result.r = vector.r;
		result.g = vector.g;
		result.b = vector.b;
		result.a = vector.a;
		return result;
	}

	glm::mat4 convert_to_glm(const aiMatrix4x4& transform) {
		glm::mat4 local(1);
		local[0] = glm::vec4(transform.a1, transform.b1, transform.c1, transform.d1);
		local[1] = glm::vec4(transform.a2, transform.b2, transform.c2, transform.d2);
		local[2] = glm::vec4(transform.a3, transform.b3, transform.c3, transform.d3);
		local[3] = glm::vec4(transform.a4, transform.b4, transform.c4, transform.d4);
		return local;
	}

	struct trs_result { glm::vec3 position; glm::vec3 rotation_deg; glm::vec3 scale; };

	trs_result decompose_aimatrix(const aiMatrix4x4& m) {
		aiVector3D pos, scale;
		aiQuaternion rot;
		m.Decompose(scale, rot, pos);
		glm::quat q = convert_to_glm(rot);
		glm::vec3 euler_rad = glm::eulerAngles(q);
		return { convert_to_glm(pos), glm::degrees(euler_rad), convert_to_glm(scale) };
	}

	void log_material_texture(const aiScene* scene, aiMaterial* mat, aiTextureType type, std::string_view name)
	{
		if (mat->GetTextureCount(type) > 0)
		{
			aiString str;
			mat->GetTexture(type, 0, &str);
			std::string_view texture_name = str.C_Str();
			egLOG("loader", "{0} count: {1}, path: {2}", name, mat->GetTextureCount(type), texture_name);
		}
	}

#define TXM_LOG(type) log_material_texture(scene, material, type, #type)
}

std::shared_ptr<desc::desc_base> scn::model_importer_adapter::operator()(const res::tag& tag, const std::vector<std::byte>& data) const
{
	PROFILE_SCOPE("LoadModel.Assimp");
    // read file via ASSIMP
    Assimp::Importer importer;
	constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
	importer.SetIOHandler(new engine_assimp_resource_system_wrapper(res::get_system(), tag, data));
	const aiScene* scene = importer.ReadFile(std::string{ tag.view() }, flags);
    // check for errors
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
    {
        egLOG("scene/model/load", "ERROR::ASSIMP::{}", importer.GetErrorString());
        return {};
    }

	std::string path = std::format("memory://{0}/{1}.desc", tag.path(), tag.pure_name());
	res::tag actual_tag = res::tag{ path };
    json::object jsdata;
	jsdata["__type"] = "prefab_desc";
	process_model(scene, jsdata, tag, actual_tag);

	auto instance = desc_system.create_instance("prefab_desc", actual_tag);
	instance->deserialize(desc_system, jsdata);
	res::get_system().store(actual_tag, desc_system.serialize_to_bytes(jsdata));
	res::get_system().register_alias(tag, actual_tag);

	return instance;
}

json::value find_material_texture(const aiScene* scene, aiMaterial* mat, aiTextureType type, res::tag tag) {
	for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
	{
		aiString str;
		mat->GetTexture(type, i, &str);
		std::string_view texture_name = str.C_Str();
		if (auto* pEmbededTxm = scene->GetEmbeddedTexture(texture_name.data()))
		{
			const std::string_view embedded_filename{ pEmbededTxm->mFilename.C_Str() };
			const std::string_view embedded_format{ pEmbededTxm->achFormatHint };
			std::string embedded_path = std::format("__embedded_txm_{0}/{1}.{2}",
					tag.pure_name(),
					embedded_filename,
				    (pEmbededTxm->mHeight != 0) ?
					res::raw_image_adapter::EXTENSION :
					res::pct_adapter::EXTENSIONS[0]
			);
			res::tag embedded_tag = res::tag(res::tag::memory, embedded_path);

			if (!res::get_system().exists(embedded_tag)) {
				glm::ivec2 size;
				int channel = 4;
				std::vector<std::byte> data_byte;

				if (pEmbededTxm->mHeight != 0) {
					size.x = pEmbededTxm->mWidth;
					size.y = pEmbededTxm->mHeight;

					res::raw_image_adapter::raw_image_header header;
					header.size = size;
					header.channels = channel;
					data_byte.resize(res::raw_image_adapter::HEADER_SIZE);
					std::memcpy(data_byte.data(), &header, res::raw_image_adapter::HEADER_SIZE);
					std::copy((std::byte*)pEmbededTxm->pcData, (std::byte*)pEmbededTxm->pcData + (size.x * size.y * channel), std::back_inserter(data_byte));
					res::get_system().store(embedded_tag, data_byte);
				}
				else {
					data_byte.reserve(pEmbededTxm->mWidth);
					std::copy((std::byte*)pEmbededTxm->pcData, (std::byte*)pEmbededTxm->pcData + pEmbededTxm->mWidth, std::back_inserter(data_byte));
					res::get_system().store(embedded_tag, data_byte);
				}
			}

			res::tag desc_tag = res::tag{ res::tag::memory, std::format("{0}{1}.txm.desc", embedded_tag.path(), embedded_tag.pure_name())};

			if (!res::get_system().exists(desc_tag)) {
				json::object desc;
				desc["__type"] = "texture_2d_desc";
				desc["__parent"] = "res://base_texture.desc";
				desc["name"] = embedded_tag.pure_name();
				desc["data"] = json::value_from(embedded_tag);

				std::string data = json::serialize(desc);
				std::vector<std::byte> desc_data(data.size());
				std::memcpy(desc_data.data(), data.data(), data.size());
				res::get_system().store(desc_tag, desc_data);
			}
			return json::value_from(desc_tag);
		}

		res::tag desc_tag = res::tag{ res::tag::memory, std::format("{0}{1}.txm.desc", tag.path(), texture_name) };

		if (!res::get_system().exists(desc_tag)) {
			json::object desc;
			desc["__type"] = "texture_2d_desc";
			desc["__parent"] = "res://base_texture.desc";
			desc["name"] = texture_name;
			desc["data"] = json::value_from(tag + res::tag::make(texture_name));

			std::string data = json::serialize(desc);
			std::vector<std::byte> desc_data(data.size());
				std::memcpy(desc_data.data(), data.data(), data.size());
				res::get_system().store(desc_tag, desc_data);
		}
		return json::value_from(desc_tag);
	}

	return {};
}

json::value process_material(const aiScene* scene, aiMaterial* material, res::tag tag)
{
	TXM_LOG(aiTextureType_DIFFUSE);
	TXM_LOG(aiTextureType_SPECULAR);
	TXM_LOG(aiTextureType_HEIGHT);
	TXM_LOG(aiTextureType_NORMALS);
	TXM_LOG(aiTextureType_AMBIENT);
	TXM_LOG(aiTextureType_BASE_COLOR);
	TXM_LOG(aiTextureType_EMISSIVE);
	TXM_LOG(aiTextureType_CLEARCOAT);
	TXM_LOG(aiTextureType_SHININESS);
	TXM_LOG(aiTextureType_OPACITY);
	TXM_LOG(aiTextureType_LIGHTMAP);
	TXM_LOG(aiTextureType_REFLECTION);
	TXM_LOG(aiTextureType_NORMAL_CAMERA);
	TXM_LOG(aiTextureType_EMISSION_COLOR);
	TXM_LOG(aiTextureType_METALNESS);
	TXM_LOG(aiTextureType_DIFFUSE_ROUGHNESS);
	TXM_LOG(aiTextureType_AMBIENT_OCCLUSION);
	TXM_LOG(aiTextureType_SHEEN);
	TXM_LOG(aiTextureType_TRANSMISSION);

	json::object jsmaterial;
	jsmaterial["__type"] = "material_desc";
	jsmaterial["__parent"] = "res://base_material.desc";
	jsmaterial["name"] = material->GetName().C_Str();
	json::value diffuse_tag = find_material_texture(scene, material, aiTextureType_DIFFUSE, tag);
	json::array samplers;
	json::array defines{"LIGHTS_ENABLED"};

	if (!diffuse_tag.is_null()) {
		samplers.push_back(diffuse_tag);
		defines.push_back("USE_TXM_AS_DIFFUSE");
	} else {
		diffuse_tag = find_material_texture(scene, material, aiTextureType_BASE_COLOR, tag);
		if (!diffuse_tag.is_null()) {
			samplers.push_back(diffuse_tag);
			defines.push_back("USE_TXM_AS_DIFFUSE");
		}
	}

	if (json::value spec_tag = find_material_texture(scene, material, aiTextureType_SPECULAR, tag); !spec_tag.is_null()) {
		samplers.resize(2);
		samplers[1] = spec_tag;
		defines.push_back("USE_SPECULAR_MAP");
	}

	if (json::value normal_tag = find_material_texture(scene, material, aiTextureType_NORMALS, tag); !normal_tag.is_null()) {
		samplers.resize(3);
		samplers[2] = normal_tag;
		defines.push_back("USE_NORMAL_MAP");
	}

	if (!samplers.empty()) {
		jsmaterial["samplers"] = samplers;
	}

	json::object uniforms;
	aiColor4D color;
	if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color)) {
		uniforms["albedo"] = json::value_from(glm::vec4(color.r, color.g, color.b, color.a));
	}

	if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_EMISSIVE, color)) {
		uniforms["emissive"] = json::value_from(glm::vec4(color.r, color.g, color.b, color.a));
	}

	float shininess = 0;
	if (AI_SUCCESS == material->Get(AI_MATKEY_SHININESS, shininess)) {
		uniforms["shininess"] = json::value_from(shininess);
	}

	if (!uniforms.empty()) {
		jsmaterial["uniforms"] = uniforms;
	}

	jsmaterial["defines"] = defines;
	jsmaterial["queue"] = "mix";
	jsmaterial["shader_fragment"] = "res://shaders/mix_opaque_trans_scene.frag";
	return jsmaterial;
}

void store_data(std::vector<std::byte>& data, const auto& value)
{
	int begin = data.size();
	data.resize(data.size() + sizeof(value));
	std::memcpy((data.data() + begin), &value, sizeof(value));
}

void process_mesh_geometry(const aiScene* scene, const aiMesh* mesh, rnd::geometry_desc& geometry, res::tag tag)
{
	for (unsigned int i = 0; i < mesh->mNumVertices; i++)
	{
		ASSERT_MSG(0 == geometry.layout.get_element_offset("position"), "Offset position mismatch");
		size_t stride = sizeof(glm::vec3);
		size_t begin = geometry.vertices.size();

		store_data(geometry.vertices, convert_to_glm(mesh->mVertices[i]));
		if (mesh->HasNormals())
		{
			ASSERT_MSG(stride == geometry.layout.get_element_offset("normal"), "Offset normal mismatch");
			stride += sizeof(glm::vec3);
			store_data(geometry.vertices, convert_to_glm(mesh->mNormals[i]));
		}
		if (mesh->HasTextureCoords(0))
		{
			ASSERT_MSG(stride == geometry.layout.get_element_offset("uv"), "Offset uv mismatch");
			stride += sizeof(glm::vec2);
			store_data(geometry.vertices, (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][i]));
		}
		if (mesh->HasTangentsAndBitangents())
		{
			ASSERT_MSG(stride == geometry.layout.get_element_offset("tangent"), "Offset tangent mismatch");
			ASSERT_MSG((stride + sizeof(glm::vec3)) == geometry.layout.get_element_offset("bitangent"), "Offset bitangent mismatch");
			stride += sizeof(glm::vec3) * 2;
			store_data(geometry.vertices, convert_to_glm(mesh->mTangents[i]));
			store_data(geometry.vertices, convert_to_glm(mesh->mBitangents[i]));
		}

		ASSERT_MSG(stride == geometry.layout.get_stride(), "Stride mismatch");
		ASSERT_MSG(stride == (geometry.vertices.size() - begin), "Stride mismatch2");
	}

	for (unsigned int i = 0; i < mesh->mNumFaces; i++)
	{
		aiFace face = mesh->mFaces[i];
		for (unsigned int j = 0; j < face.mNumIndices; j++) {
			geometry.indices.push_back(face.mIndices[j]);
		}

		for (unsigned int j = 0; j < face.mNumIndices; j += 3) {
			auto uv1 = (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][face.mIndices[j]]);
			auto uv2 = (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][face.mIndices[j + 1]]);
			auto uv3 = (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][face.mIndices[j + 2]]);

			ds::bbox box;
			ds::expand(box, uv1);
			ds::expand(box, uv2);
			ds::expand(box, uv3);

			geometry.bounds.push_back({ box, static_cast<uint32_t>(geometry.bounds.size()) });
		}
	}
}

// Builds skinning weights for this mesh in the format expected by skinning_manager:
// result[vertex_idx] = [bone_idx_0, bone_idx_1, ..., weight_0_as_uint32, weight_1_as_uint32, ...]
void build_mesh_skin_weights(
	const aiMesh* mesh,
	const std::unordered_map<std::string, int>& bone_index_map,
	std::size_t vx_begin,
	std::vector<std::vector<uint32_t>>& out_weights)
{
	std::size_t num_vx = mesh->mNumVertices;
	// per-vertex list of (bone_idx, weight) pairs
	std::vector<std::vector<std::pair<int, float>>> vtx_bones(num_vx);

	for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi) {
		aiBone* b = mesh->mBones[bi];
		std::string bname = b->mName.C_Str();
		int bidx = 0;
		if (auto it = bone_index_map.find(bname); it != bone_index_map.end())
			bidx = it->second;

		for (unsigned int wi = 0; wi < b->mNumWeights; ++wi) {
			unsigned int vi = b->mWeights[wi].mVertexId;
			float w = b->mWeights[wi].mWeight;
			if (vi < vtx_bones.size())
				vtx_bones[vi].emplace_back(bidx, w);
		}
	}

	std::size_t required_size = vx_begin + num_vx;
	if (out_weights.size() < required_size)
		out_weights.resize(required_size);

	for (std::size_t vi = 0; vi < num_vx; ++vi) {
		const auto& vw = vtx_bones[vi];
		std::vector<uint32_t> column(vw.size() * 2);
		for (std::size_t k = 0; k < vw.size(); ++k) {
			column[k] = static_cast<uint32_t>(vw[k].first);
			column[vw.size() + k] = std::bit_cast<uint32_t>(vw[k].second);
		}
		out_weights[vx_begin + vi] = std::move(column);
	}
}

json::object build_prefab_node(
	const aiScene* scene,
	aiNode* node,
	rnd::geometry_desc& geometry,
	res::tag geom_tag,
	const std::unordered_map<std::string, int>& bone_index_map,
	const std::unordered_set<std::string>& animated_node_names,
	std::vector<std::vector<uint32_t>>& skin_weights,
	res::tag tag,
	res::tag prefab_tag)
{
	json::object jsnode;
	std::string node_name = node->mName.C_Str();

	// Transform
	auto trs = decompose_aimatrix(node->mTransformation);
	{
		json::object transform;
		bool has_transform = false;
		if (trs.position != glm::vec3(0)) { transform["position"] = json::value_from(trs.position); has_transform = true; }
		if (trs.rotation_deg != glm::vec3(0)) { transform["rotation"] = json::value_from(trs.rotation_deg); has_transform = true; }
		if (trs.scale != glm::vec3(1)) { transform["scale"] = json::value_from(trs.scale); has_transform = true; }
		if (has_transform)
			jsnode["transform"] = transform;
	}

	json::object components;

	// Bone component
	if (auto* bone = scene->findBone(aiString(node_name))) {
		if (auto it = bone_index_map.find(node_name); it != bone_index_map.end()) {
			json::object bone_comp;
			bone_comp["offset_matrix"] = json::value_from(convert_to_glm(bone->mOffsetMatrix));
			bone_comp["index"] = it->second;
			components["bone_desc"] = bone_comp;
		}
	}

	// Animated node marker (set at import time for nodes with animation channels)
	if (animated_node_names.contains(node_name)) {
		components["animated_node_desc"] = json::object{};
	}

	if (!components.empty())
		jsnode["components"] = components;

	// Children
	json::object children;

	// Mesh children — one child per mesh attached to this node
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];

		std::size_t vx_begin = geometry.vertices.size() / geometry.layout.get_stride();
		std::size_t ind_begin = geometry.indices.size();

		process_mesh_geometry(scene, mesh, geometry, tag);

		std::size_t vx_end = geometry.vertices.size() / geometry.layout.get_stride();
		std::size_t ind_end = geometry.indices.size();

		json::object mesh_node_comp;
		mesh_node_comp["geometry"] = json::value_from(geom_tag);
		mesh_node_comp["vx_begin"] = vx_begin;
		mesh_node_comp["vx_end"] = vx_end;
		mesh_node_comp["ind_begin"] = ind_begin;
		mesh_node_comp["ind_end"] = ind_end;
		mesh_node_comp["material"] = process_material(scene, scene->mMaterials[mesh->mMaterialIndex], tag);

		json::object mesh_components;
		mesh_components["mesh_node_desc"] = mesh_node_comp;

		if (mesh->mNumBones > 0) {
			json::object skinning_comp;
			skinning_comp["bone_count"] = static_cast<int>(mesh->mNumBones);
			skinning_comp["skinning_tag"] = json::value_from(prefab_tag);
			mesh_components["skinning_desc"] = skinning_comp;

			build_mesh_skin_weights(mesh, bone_index_map, vx_begin, skin_weights);
		}

		json::object mesh_child;
		mesh_child["components"] = mesh_components;

		std::string mesh_child_name = "mesh_" + std::string(mesh->mName.C_Str());
		children[mesh_child_name] = mesh_child;
	}

	// Recursive children
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		aiNode* child_node = node->mChildren[i];
		std::string child_name = child_node->mName.C_Str();
		children[child_name] = build_prefab_node(scene, child_node, geometry, geom_tag, bone_index_map, animated_node_names, skin_weights, tag, prefab_tag);
	}

	if (!children.empty())
		jsnode["children"] = children;

	return jsnode;
}

void process_model(const aiScene* scene, json::object& data, res::tag tag, res::tag prefab_tag)
{
	// 1. Setup geometry layout from first mesh
	rnd::geometry_desc geometry;
	if (scene->HasMeshes()) {
		std::vector<rnd::driver::BufferElement> layout;
		layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position" });

		auto mesh = scene->mMeshes[0];
		if (mesh->HasNormals()) {
			layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal" });
		}
		if (mesh->HasTextureCoords(0)) {
			layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC2_F, "uv" });
		}
		if (mesh->HasTangentsAndBitangents()) {
			layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC3_F, "tangent" });
			layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC3_F, "bitangent" });
		}
		geometry.layout = rnd::driver::BufferLayout{ layout };
	}

	// 2. Pre-scan bones: bone_name → global index
	std::unordered_map<std::string, int> bone_index_map;
	int bone_idx = 0;
	for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
		for (unsigned int bi = 0; bi < scene->mMeshes[mi]->mNumBones; ++bi) {
			std::string bname = scene->mMeshes[mi]->mBones[bi]->mName.C_Str();
			if (!bone_index_map.count(bname))
				bone_index_map[bname] = bone_idx++;
		}
	}

	// 3. Pre-build animation clips: anim_name → { node_name → animation_node JSON }
	// Each clip contains all channels (nodes) for one animation.
	struct clip_data {
		double duration = 0.0;
		double ticks_per_second = 25.0;
		json::object channels; // node_name → animation_node JSON
	};
	std::unordered_map<std::string, clip_data> clip_map;

	for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai) {
		auto* anim = scene->mAnimations[ai];
		std::string anim_name = anim->mName.C_Str();

		auto& clip = clip_map[anim_name];
		clip.duration = anim->mDuration;
		clip.ticks_per_second = anim->mTicksPerSecond;

		for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci) {
			auto* chan = anim->mChannels[ci];
			std::string nname = chan->mNodeName.C_Str();

			json::array pos_keys, rot_keys, scale_keys;
			for (unsigned int k = 0; k < chan->mNumPositionKeys; ++k) {
				pos_keys.push_back(json::object{
					{"value", json::value_from(convert_to_glm(chan->mPositionKeys[k].mValue))},
					{"time", chan->mPositionKeys[k].mTime}
				});
			}
			for (unsigned int k = 0; k < chan->mNumRotationKeys; ++k) {
				rot_keys.push_back(json::object{
					{"value", json::value_from(convert_to_glm(chan->mRotationKeys[k].mValue))},
					{"time", chan->mRotationKeys[k].mTime}
				});
			}
			for (unsigned int k = 0; k < chan->mNumScalingKeys; ++k) {
				scale_keys.push_back(json::object{
					{"value", json::value_from(convert_to_glm(chan->mScalingKeys[k].mValue))},
					{"time", chan->mScalingKeys[k].mTime}
				});
			}

			json::object anim_node;
			anim_node["position"] = pos_keys;
			anim_node["rotation"] = rot_keys;
			anim_node["scale"] = scale_keys;
			clip.channels[nname] = anim_node;
		}
	}

	// 4. Geometry tag for this model
	res::tag geom_tag = res::tag{ res::tag::memory, std::format("{0}{1}.geom.desc", tag.path(), tag.pure_name()) };

	// 4b. Collect animated node names from all clips
	std::unordered_set<std::string> animated_node_names;
	for (auto const& [anim_name, clip] : clip_map) {
		for (auto const& [node_name, _] : clip.channels) {
			animated_node_names.insert(node_name);
		}
	}

	// 5. Build prefab tree + accumulate skinning weights
	std::vector<std::vector<uint32_t>> skin_weights;
	json::object prefab_root = build_prefab_node(scene, scene->mRootNode, geometry, geom_tag, bone_index_map, animated_node_names, skin_weights, tag, prefab_tag);

	// 6. Add animation_collection_desc component to root (inline clips)
	if (!clip_map.empty()) {
		json::array clips_arr;
		for (auto const& [anim_name, clip] : clip_map) {
			json::object clip_obj;
			clip_obj["__type"] = "animation_clip_desc";
			clip_obj["name"] = anim_name;
			clip_obj["duration"] = clip.duration;
			clip_obj["ticks_per_second"] = clip.ticks_per_second;
			clip_obj["channels"] = clip.channels;
			clips_arr.push_back(std::move(clip_obj));
		}

		json::object collection_comp;
		collection_comp["clips"] = std::move(clips_arr);

		if (prefab_root.contains("components")) {
			prefab_root["components"].as_object()["animation_collection_desc"] = collection_comp;
		} else {
			prefab_root["components"] = json::object{ {"animation_collection_desc", collection_comp} };
		}
	}

	// 7. Store geometry as a memory resource
	json::object jsgeometry;
	jsgeometry["__type"] = "geometry_desc";
	jsgeometry["__parent"] = "res://base_geometry.desc";
	geometry.serialize(jsgeometry);
	std::string geom_str = json::serialize(jsgeometry);
	std::vector<std::byte> geom_bytes(geom_str.size());
	std::memcpy(geom_bytes.data(), geom_str.data(), geom_str.size());
	res::get_system().store(geom_tag, geom_bytes);

	// 8. Add skin_weights_desc component to root (registered during assembly)
	if (!skin_weights.empty()) {
		json::object sw_comp;
		sw_comp["tag"] = json::value_from(prefab_tag);
		json::array sw_arr;
		sw_arr.reserve(skin_weights.size());
		for (auto const& column : skin_weights) {
			json::array vtx_arr;
			vtx_arr.reserve(column.size());
			for (uint32_t v : column) {
				vtx_arr.push_back(static_cast<uint64_t>(v));
			}
			sw_arr.push_back(std::move(vtx_arr));
		}
		sw_comp["weights"] = std::move(sw_arr);

		if (prefab_root.contains("components")) {
			prefab_root["components"].as_object()["skin_weights_desc"] = sw_comp;
		} else {
			prefab_root["components"] = json::object{ {"skin_weights_desc", sw_comp} };
		}
	}

	// Copy prefab fields into data (keeping __type already set by caller)
	for (auto& kv : prefab_root) {
		data[kv.key()] = kv.value();
	}
}
