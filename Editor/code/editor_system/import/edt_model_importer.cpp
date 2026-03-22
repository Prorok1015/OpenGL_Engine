#include "edt_model_importer.h"
#include "edt_filesystem_assimp_io.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "rnd_render_system.h"
#include "skinning/rnd_skinning_manager.h"
#include "geom/rnd_geometry_desc.h"
#include "adapters/res_pct_adapter.h"
#include "scn_glm_json_convert.h"
#include "scn_mesh_nodes.hpp"

#include <fstream>
#include <unordered_set>

namespace json = boost::json;

// ─── Helpers (same as in scn_model_importer_adapter.cpp) ─────────────────────

namespace {

glm::quat convert_to_glm(const aiQuaternion& v)
{
	return glm::quat(v.w, v.x, v.y, v.z);
}

glm::vec3 convert_to_glm(const aiVector3D& v)
{
	return glm::vec3(v.x, v.y, v.z);
}

glm::vec4 convert_to_glm(const aiColor4D& v)
{
	return glm::vec4(v.r, v.g, v.b, v.a);
}

glm::mat4 convert_to_glm(const aiMatrix4x4& m)
{
	glm::mat4 local(1);
	local[0] = glm::vec4(m.a1, m.b1, m.c1, m.d1);
	local[1] = glm::vec4(m.a2, m.b2, m.c2, m.d2);
	local[2] = glm::vec4(m.a3, m.b3, m.c3, m.d3);
	local[3] = glm::vec4(m.a4, m.b4, m.c4, m.d4);
	return local;
}

struct trs_result { glm::vec3 position; glm::vec3 rotation_deg; glm::vec3 scale; };

trs_result decompose_aimatrix(const aiMatrix4x4& m)
{
	aiVector3D pos, scale;
	aiQuaternion rot;
	m.Decompose(scale, rot, pos);
	glm::quat q = convert_to_glm(rot);
	glm::vec3 euler_rad = glm::eulerAngles(q);
	return { convert_to_glm(pos), glm::degrees(euler_rad), convert_to_glm(scale) };
}

void store_data(std::vector<std::byte>& data, const auto& value)
{
	int begin = data.size();
	data.resize(data.size() + sizeof(value));
	std::memcpy((data.data() + begin), &value, sizeof(value));
}

void log_material_texture(const aiScene* scene, aiMaterial* mat, aiTextureType type, std::string_view name)
{
	if (mat->GetTextureCount(type) > 0) {
		aiString str;
		mat->GetTexture(type, 0, &str);
		egLOG("edt/import", "{0} count: {1}, path: {2}", name, mat->GetTextureCount(type), str.C_Str());
	}
}

#define TXM_LOG(type) log_material_texture(scene, material, type, #type)

// ─── Texture handling ────────────────────────────────────────────────────────

json::value find_material_texture(
	const aiScene* scene,
	aiMaterial* mat,
	aiTextureType type,
	const std::string& mem_prefix,
	const std::filesystem::path& model_dir,
	res::resource_system& res_sys,
	std::vector<res::tag>& out_tags)
{
	for (unsigned int i = 0; i < mat->GetTextureCount(type); i++) {
		aiString str;
		mat->GetTexture(type, i, &str);
		std::string_view texture_name = str.C_Str();

		if (auto* pEmbTxm = scene->GetEmbeddedTexture(texture_name.data())) {
			const std::string_view emb_name{ pEmbTxm->mFilename.C_Str() };
			const std::string_view emb_fmt{ pEmbTxm->achFormatHint };

			std::string emb_path = std::format("__embedded_txm/{0}.{1}",
				emb_name,
				(pEmbTxm->mHeight != 0) ?
					res::raw_image_adapter::EXTENSION :
					res::pct_adapter::EXTENSIONS[0]);

			res::tag emb_tag = res::tag(res::tag::memory, mem_prefix + emb_path);

			if (!res_sys.exists(emb_tag)) {
				std::vector<std::byte> data_byte;

				if (pEmbTxm->mHeight != 0) {
					glm::ivec2 size(pEmbTxm->mWidth, pEmbTxm->mHeight);
					int channel = 4;

					res::raw_image_adapter::raw_image_header header;
					header.size = size;
					header.channels = channel;
					data_byte.resize(res::raw_image_adapter::HEADER_SIZE);
					std::memcpy(data_byte.data(), &header, res::raw_image_adapter::HEADER_SIZE);
					std::copy(
						(std::byte*)pEmbTxm->pcData,
						(std::byte*)pEmbTxm->pcData + (size.x * size.y * channel),
						std::back_inserter(data_byte));
				} else {
					data_byte.reserve(pEmbTxm->mWidth);
					std::copy(
						(std::byte*)pEmbTxm->pcData,
						(std::byte*)pEmbTxm->pcData + pEmbTxm->mWidth,
						std::back_inserter(data_byte));
				}

				res_sys.store(emb_tag, data_byte);
			}
			out_tags.push_back(emb_tag);

			res::tag desc_tag = res::tag{ res::tag::memory,
				std::format("{0}{1}.txm.desc", emb_tag.path(), emb_tag.pure_name()) };

			if (!res_sys.exists(desc_tag)) {
				json::object desc;
				desc["__type"] = "texture_2d_desc";
				desc["__parent"] = "res://base_texture.desc";
				desc["name"] = emb_tag.pure_name();
				desc["data"] = json::value_from(emb_tag);

				std::string s = json::serialize(desc);
				std::vector<std::byte> desc_data(s.size());
				std::memcpy(desc_data.data(), s.data(), s.size());
				res_sys.store(desc_tag, desc_data);
			}
			out_tags.push_back(desc_tag);

			return json::value_from(desc_tag);
		}

		// External texture file — read from model's directory and store in memory://
		res::tag tex_data_tag = res::tag(res::tag::memory, mem_prefix + "textures/" + std::string(texture_name));

		if (!res_sys.exists(tex_data_tag)) {
			std::filesystem::path tex_path = model_dir / std::string(texture_name);
			std::ifstream tex_in(tex_path, std::ios::binary | std::ios::ate);
			if (tex_in) {
				auto tex_size = static_cast<std::streamsize>(tex_in.tellg());
				tex_in.seekg(0);
				std::vector<std::byte> tex_bytes(static_cast<size_t>(tex_size));
				tex_in.read(reinterpret_cast<char*>(tex_bytes.data()), tex_size);
				res_sys.store(tex_data_tag, tex_bytes);
			} else {
				egLOG_WARN("edt/import", "External texture not found: {}", tex_path.string());
			}
		}
		out_tags.push_back(tex_data_tag);

		res::tag desc_tag = res::tag{ res::tag::memory,
			std::format("{0}textures/{1}.txm.desc", mem_prefix, texture_name) };

		if (!res_sys.exists(desc_tag)) {
			json::object desc;
			desc["__type"] = "texture_2d_desc";
			desc["__parent"] = "res://base_texture.desc";
			desc["name"] = texture_name;
			desc["data"] = json::value_from(tex_data_tag);

			std::string s = json::serialize(desc);
			std::vector<std::byte> desc_data(s.size());
			std::memcpy(desc_data.data(), s.data(), s.size());
			res_sys.store(desc_tag, desc_data);
		}
		out_tags.push_back(desc_tag);

		return json::value_from(desc_tag);
	}

	return {};
}

json::value process_material(
	const aiScene* scene,
	aiMaterial* material,
	const std::string& mem_prefix,
	const std::filesystem::path& model_dir,
	res::resource_system& res_sys,
	std::vector<res::tag>& out_tags)
{
	TXM_LOG(aiTextureType_DIFFUSE);
	TXM_LOG(aiTextureType_SPECULAR);
	TXM_LOG(aiTextureType_HEIGHT);
	TXM_LOG(aiTextureType_NORMALS);
	TXM_LOG(aiTextureType_BASE_COLOR);

	std::string mat_name = material->GetName().C_Str();

	json::object jsmaterial;
	jsmaterial["__type"] = "material_desc";
	jsmaterial["__parent"] = "res://base_material.desc";
	jsmaterial["name"] = mat_name;

	json::value diffuse_tag = find_material_texture(scene, material, aiTextureType_DIFFUSE, mem_prefix, model_dir, res_sys, out_tags);
	json::array samplers;
	json::array defines{"LIGHTS_ENABLED"};

	if (!diffuse_tag.is_null()) {
		samplers.push_back(diffuse_tag);
		defines.push_back("USE_TXM_AS_DIFFUSE");
	} else {
		diffuse_tag = find_material_texture(scene, material, aiTextureType_BASE_COLOR, mem_prefix, model_dir, res_sys, out_tags);
		if (!diffuse_tag.is_null()) {
			samplers.push_back(diffuse_tag);
			defines.push_back("USE_TXM_AS_DIFFUSE");
		}
	}

	json::value spec = find_material_texture(scene, material, aiTextureType_SPECULAR, mem_prefix, model_dir, res_sys, out_tags);
	if (!spec.is_null()) {
		// Ensure slot 1 exists (slot 0 = diffuse or placeholder)
		while (samplers.size() < 1) samplers.push_back(nullptr);
		samplers.push_back(spec);
		defines.push_back("USE_SPECULAR_MAP");
	}

	json::value norm = find_material_texture(scene, material, aiTextureType_NORMALS, mem_prefix, model_dir, res_sys, out_tags);
	if (!norm.is_null()) {
		// Ensure slots 0-1 exist before adding normal at slot 2
		while (samplers.size() < 2) samplers.push_back(nullptr);
		samplers.push_back(norm);
		defines.push_back("USE_NORMAL_MAP");
	}

	if (!samplers.empty())
		jsmaterial["samplers"] = samplers;

	json::object uniforms;
	aiColor4D color;
	if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color))
		uniforms["albedo"] = json::value_from(glm::vec4(color.r, color.g, color.b, color.a));
	if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_EMISSIVE, color))
		uniforms["emissive"] = json::value_from(glm::vec4(color.r, color.g, color.b, color.a));

	float shininess = 0;
	if (AI_SUCCESS == material->Get(AI_MATKEY_SHININESS, shininess))
		uniforms["shininess"] = json::value_from(shininess);

	if (!uniforms.empty())
		jsmaterial["uniforms"] = uniforms;

	jsmaterial["defines"] = defines;
	jsmaterial["queue"] = "mix";
	jsmaterial["shader_fragment"] = "res://shaders/mix_opaque_trans_scene.frag";

	// Store as separate memory:// resource instead of inlining in prefab
	res::tag mat_tag = res::tag{ res::tag::memory,
		std::format("{0}materials/{1}.mat.desc", mem_prefix, mat_name) };

	if (!res_sys.exists(mat_tag)) {
		std::string s = json::serialize(jsmaterial);
		std::vector<std::byte> data(s.size());
		std::memcpy(data.data(), s.data(), s.size());
		res_sys.store(mat_tag, data);
	}
	out_tags.push_back(mat_tag);

	return json::value_from(mat_tag);
}

// ─── Geometry ────────────────────────────────────────────────────────────────

void process_mesh_geometry(const aiScene* scene, const aiMesh* mesh, rnd::geometry_desc& geometry)
{
	// Validate layout offsets once before the loop (PERF-06)
	{
		size_t expected_stride = sizeof(glm::vec3); // position
		ASSERT_MSG(0 == geometry.layout.get_element_offset("position"), "Offset position mismatch");
		if (mesh->HasNormals()) {
			ASSERT_MSG(expected_stride == geometry.layout.get_element_offset("normal"), "Offset normal mismatch");
			expected_stride += sizeof(glm::vec3);
		}
		if (mesh->HasTextureCoords(0)) {
			ASSERT_MSG(expected_stride == geometry.layout.get_element_offset("uv"), "Offset uv mismatch");
			expected_stride += sizeof(glm::vec2);
		}
		if (mesh->HasTangentsAndBitangents()) {
			ASSERT_MSG(expected_stride == geometry.layout.get_element_offset("tangent"), "Offset tangent mismatch");
			ASSERT_MSG((expected_stride + sizeof(glm::vec3)) == geometry.layout.get_element_offset("bitangent"), "Offset bitangent mismatch");
			expected_stride += sizeof(glm::vec3) * 2;
		}
		ASSERT_MSG(expected_stride == geometry.layout.get_stride(), "Stride mismatch");
	}

	for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
		size_t begin = geometry.vertices.size();

		store_data(geometry.vertices, convert_to_glm(mesh->mVertices[i]));
		if (mesh->HasNormals()) {
			store_data(geometry.vertices, convert_to_glm(mesh->mNormals[i]));
		}
		if (mesh->HasTextureCoords(0)) {
			store_data(geometry.vertices, (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][i]));
		}
		if (mesh->HasTangentsAndBitangents()) {
			store_data(geometry.vertices, convert_to_glm(mesh->mTangents[i]));
			store_data(geometry.vertices, convert_to_glm(mesh->mBitangents[i]));
		}
	}

	const bool has_uvs = mesh->HasTextureCoords(0);

	for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
		const aiFace& face = mesh->mFaces[i];
		for (unsigned int j = 0; j < face.mNumIndices; j++)
			geometry.indices.push_back(face.mIndices[j]);

		if (has_uvs) {
			for (unsigned int j = 0; j + 2 < face.mNumIndices; j += 3) {
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
}

// ─── Skinning weights ────────────────────────────────────────────────────────

void build_mesh_skin_weights(
	const aiMesh* mesh,
	const std::unordered_map<std::string, int>& bone_index_map,
	std::size_t vx_begin,
	std::vector<std::vector<uint32_t>>& out_weights)
{
	std::size_t num_vx = mesh->mNumVertices;
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

// ─── Prefab node builder ─────────────────────────────────────────────────────

json::object build_prefab_node(
	const aiScene* scene,
	aiNode* node,
	rnd::geometry_desc& geometry,
	res::tag geom_tag,
	const std::unordered_map<std::string, int>& bone_index_map,
	const std::unordered_map<std::string, json::object>& node_kf_map,
	std::vector<std::vector<uint32_t>>& skin_weights,
	const std::string& mem_prefix,
	const std::filesystem::path& model_dir,
	res::resource_system& res_sys,
	std::vector<res::tag>& out_tags,
	res::tag prefab_tag)
{
	json::object jsnode;
	std::string node_name = node->mName.C_Str();

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

	if (auto* bone = scene->findBone(aiString(node_name))) {
		if (auto it = bone_index_map.find(node_name); it != bone_index_map.end()) {
			json::object bone_comp;
			bone_comp["offset_matrix"] = json::value_from(convert_to_glm(bone->mOffsetMatrix));
			bone_comp["index"] = it->second;
			components["bone_desc"] = bone_comp;
		}
	}

	if (auto it = node_kf_map.find(node_name); it != node_kf_map.end()) {
		json::object kf_comp;
		kf_comp["keyframes"] = it->second;
		components["keyframes_desc"] = kf_comp;
	}

	if (!components.empty())
		jsnode["components"] = components;

	json::object children;

	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];

		std::size_t vx_begin = geometry.vertices.size() / geometry.layout.get_stride();
		std::size_t ind_begin = geometry.indices.size();

		process_mesh_geometry(scene, mesh, geometry);

		std::size_t vx_end = geometry.vertices.size() / geometry.layout.get_stride();
		std::size_t ind_end = geometry.indices.size();

		json::object mesh_node_comp;
		mesh_node_comp["geometry"] = json::value_from(geom_tag);
		mesh_node_comp["vx_begin"] = vx_begin;
		mesh_node_comp["vx_end"] = vx_end;
		mesh_node_comp["ind_begin"] = ind_begin;
		mesh_node_comp["ind_end"] = ind_end;
		mesh_node_comp["material"] = process_material(scene, scene->mMaterials[mesh->mMaterialIndex], mem_prefix, model_dir, res_sys, out_tags);

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

	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		aiNode* child_node = node->mChildren[i];
		std::string child_name = child_node->mName.C_Str();
		children[child_name] = build_prefab_node(
			scene, child_node, geometry, geom_tag, bone_index_map,
			node_kf_map, skin_weights, mem_prefix, model_dir, res_sys, out_tags, prefab_tag);
	}

	if (!children.empty())
		jsnode["children"] = children;

	return jsnode;
}

} // anonymous namespace

// ─── model_importer implementation ───────────────────────────────────────────

edt::model_importer::model_importer(desc::desc_system& ds, res::resource_system& rs, rnd::render_system& rnd)
	: m_desc(ds)
	, m_res(rs)
	, m_rnd(rnd)
{
}

edt::import_result edt::model_importer::import(const std::filesystem::path& abs_path)
{
	auto intermediate = import_background(abs_path);
	if (!intermediate.error.empty()) {
		import_result result;
		result.source_path = abs_path;
		return result;
	}
	return finalize_on_main(std::move(intermediate));
}

edt::import_intermediate edt::model_importer::import_background(const std::filesystem::path& abs_path)
{
	import_intermediate im;
	im.source_path = abs_path;

	// 1. Read file from disk
	std::ifstream in(abs_path, std::ios::binary | std::ios::ate);
	if (!in) {
		im.error = std::format("Failed to open file: {}", abs_path.string());
		egLOG_ERROR("edt/import", "{}", im.error);
		return im;
	}

	auto file_size = static_cast<std::streamsize>(in.tellg());
	in.seekg(0);
	std::vector<std::byte> file_data(static_cast<size_t>(file_size));
	in.read(reinterpret_cast<char*>(file_data.data()), file_size);
	in.close();

	// 2. Import via Assimp with filesystem IO
	Assimp::Importer importer;
	constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
	importer.SetIOHandler(new edt::filesystem_assimp_io(abs_path, file_data));
	const aiScene* scene = importer.ReadFile(abs_path.string(), flags);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
		im.error = std::format("ASSIMP error: {}", importer.GetErrorString());
		egLOG_ERROR("edt/import", "{}", im.error);
		return im;
	}

	// 3. Determine memory:// prefix for this import
	std::string model_name = abs_path.stem().string();
	std::string mem_prefix = model_name + "/";

	// 4. Setup geometry
	rnd::geometry_desc geometry;
	if (scene->HasMeshes()) {
		std::vector<rnd::driver::BufferElement> layout;
		layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position" });

		auto mesh = scene->mMeshes[0];
		if (mesh->HasNormals())
			layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal" });
		if (mesh->HasTextureCoords(0))
			layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC2_F, "uv" });
		if (mesh->HasTangentsAndBitangents()) {
			layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC3_F, "tangent" });
			layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC3_F, "bitangent" });
		}
		geometry.layout = rnd::driver::BufferLayout{ layout };
	}

	// 5. Pre-scan bones
	std::unordered_map<std::string, int> bone_index_map;
	int bone_idx = 0;
	for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
		for (unsigned int bi = 0; bi < scene->mMeshes[mi]->mNumBones; ++bi) {
			std::string bname = scene->mMeshes[mi]->mBones[bi]->mName.C_Str();
			if (!bone_index_map.count(bname))
				bone_index_map[bname] = bone_idx++;
		}
	}

	// 6. Pre-build keyframes
	std::unordered_map<std::string, json::object> node_kf_map;
	json::object animations_obj;

	for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai) {
		auto* anim = scene->mAnimations[ai];
		std::string anim_name = anim->mName.C_Str();

		json::object anim_entry;
		anim_entry["duration"] = anim->mDuration;
		anim_entry["ticks_per_second"] = anim->mTicksPerSecond;
		animations_obj[anim_name] = anim_entry;

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
			node_kf_map[nname][anim_name] = anim_node;
		}
	}

	// 7. Geometry tag
	res::tag geom_tag = res::tag{ res::tag::memory, mem_prefix + model_name + ".geom.desc" };
	im.geom_tag = geom_tag;

	// 8. Build prefab tree + skinning weights
	std::filesystem::path model_dir = abs_path.parent_path();
	res::tag prefab_tag = res::tag{ res::tag::memory, mem_prefix + model_name + ".prefab.desc" };
	json::object prefab_root = build_prefab_node(
		scene, scene->mRootNode, geometry, geom_tag, bone_index_map,
		node_kf_map, im.skin_weights, mem_prefix, model_dir, m_res, im.all_tags, prefab_tag);

	// 9. Add animations_desc to root
	if (scene->HasAnimations()) {
		json::object anim_comp;
		anim_comp["animations"] = animations_obj;

		if (prefab_root.contains("components") && prefab_root["components"].is_object())
			prefab_root["components"].as_object()["animations_desc"] = anim_comp;
		else
			prefab_root["components"] = json::object{ {"animations_desc", anim_comp} };
	}

	// 9b. Add skin_weights_desc to root (registered during assembly)
	if (!im.skin_weights.empty()) {
		json::object sw_comp;
		sw_comp["tag"] = json::value_from(prefab_tag);
		json::array sw_arr;
		sw_arr.reserve(im.skin_weights.size());
		for (auto const& column : im.skin_weights) {
			json::array vtx_arr;
			vtx_arr.reserve(column.size());
			for (uint32_t v : column) {
				vtx_arr.push_back(static_cast<uint64_t>(v));
			}
			sw_arr.push_back(std::move(vtx_arr));
		}
		sw_comp["weights"] = std::move(sw_arr);

		if (prefab_root.contains("components") && prefab_root["components"].is_object())
			prefab_root["components"].as_object()["skin_weights_desc"] = sw_comp;
		else
			prefab_root["components"] = json::object{ {"skin_weights_desc", sw_comp} };
	}

	// 10. Store geometry (res_sys.store is mutex-protected)
	json::object jsgeometry;
	jsgeometry["__type"] = "geometry_desc";
	jsgeometry["__parent"] = "res://base_geometry.desc";
	geometry.serialize(jsgeometry);
	std::string geom_str = json::serialize(jsgeometry);
	std::vector<std::byte> geom_bytes(geom_str.size());
	std::memcpy(geom_bytes.data(), geom_str.data(), geom_str.size());
	m_res.store(geom_tag, geom_bytes);
	im.all_tags.push_back(geom_tag);

	// 11. Build and store prefab JSON
	json::object jsdata;
	jsdata["__type"] = "prefab_desc";
	for (auto& kv : prefab_root)
		jsdata[kv.key()] = kv.value();

	im.prefab_tag = prefab_tag;

	m_res.store(prefab_tag, m_desc.serialize_to_bytes(jsdata));
	im.all_tags.push_back(prefab_tag);

	// Save prefab JSON for main-thread finalization (owned copy)
	im.prefab_json = std::move(jsdata);

	// Deduplicate tags (materials/textures shared by multiple meshes)
	{
		std::unordered_set<res::tag> seen;
		std::erase_if(im.all_tags, [&seen](const res::tag& t) {
			return !seen.insert(t).second;
		});
	}

	egLOG("edt/import", "Background import done: '{}' → {} memory resources", abs_path.filename().string(), im.all_tags.size());
	for (const auto& t : im.all_tags) {
		egLOG("edt/import", "  tag: {}", t.view());
	}
	return im;
}

edt::import_result edt::model_importer::finalize_on_main(import_intermediate&& im)
{
	import_result result;
	result.source_path = std::move(im.source_path);
	result.root_tag = im.prefab_tag;
	result.all_tags = std::move(im.all_tags);

	// desc_sys.create_instance + deserialize — main thread only
	auto instance = m_desc.create_instance("prefab_desc", im.prefab_tag);
	if (instance) {
		instance->deserialize(m_desc, im.prefab_json);
		result.prefab = ds::polymorphic_pointer_cast<scn::prefab_desc>(instance);
	}

	return result;
}
