#include "scn_model_importer_adapter.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "res_system.h"

#include "geom/rnd_geometry_desc.h"
#include "adapters/res_pct_adapter.h"

#include "scn_glm_json_convert.h"
#include "res_mesh.hpp"

#include "scn_assimp_resource_system_wrapper.h"

#include "ds_svg_writer.hpp"

void process_model(const aiScene* scene, json::object& data, res::tag tag);

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

#define TXM_LOG(type) log_material_texture(scene, material, type, ###type)
}

std::shared_ptr<desc::desc_resource> scn::model_importer_adapter::operator()(const res::tag& tag, const std::vector<std::byte>& data) const
{
    // read file via ASSIMP
    Assimp::Importer importer;
	constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
	importer.SetIOHandler(new engine_assimp_resource_system_wrapper(res::get_system(), tag, data));
	const aiScene* scene = importer.ReadFile(std::string{ tag.get_full() }, flags);
    // check for errors
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) // if is Not Zero
    {
        egLOG("scene/model/load", "ERROR::ASSIMP::{}", importer.GetErrorString());
        return {};
    }

    json::object jsdata;
	process_model(scene, jsdata, tag);

	return std::make_shared<desc::desc_resource>(tag, jsdata);
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
			std::string embedded_path = std::vformat("__embedded_txm_{0}/{1}.{2}", 
				std::make_format_args(
					tag.pure_name(), 
					embedded_filename,
				    (pEmbededTxm->mHeight != 0) ? 
					res::raw_image_adapter::EXTENSION : 
					res::pct_adapter::EXTENSIONS[0]
				)
			);
			res::tag embedded_tag = res::tag(res::tag::memory, embedded_path);

			if (!res::get_system().memory_resolver_.is_exist(embedded_tag)) {
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
					res::get_system().memory_resolver_.add_memory_resource(embedded_tag, data_byte);
				}
				else {
					data_byte.reserve(pEmbededTxm->mWidth);
					std::copy((std::byte*)pEmbededTxm->pcData, (std::byte*)pEmbededTxm->pcData + pEmbededTxm->mWidth, std::back_inserter(data_byte));
					res::get_system().memory_resolver_.add_memory_resource(embedded_tag, data_byte);
				}
			}

			res::tag desc_tag = res::tag{ res::tag::memory, std::vformat("{0}{1}.txm.desc", std::make_format_args(embedded_tag.path(), embedded_tag.pure_name()))};

			if (!res::get_system().memory_resolver_.is_exist(desc_tag)) {
				json::object desc;
				desc["__parent"] = "res://base_texture.desc";
				desc["name"] = embedded_tag.pure_name();
				desc["data"] = json::value_from(embedded_tag);

				std::string data = json::serialize(desc);
				std::vector<std::byte> desc_data(data.size());
				std::memcpy(desc_data.data(), data.data(), data.size());
				res::get_system().memory_resolver_.add_memory_resource(desc_tag, desc_data);
			}
			return json::value_from(desc_tag);
		}
		
		res::tag desc_tag = res::tag{ res::tag::memory, std::vformat("{0}{1}.txm.desc", std::make_format_args(tag.path(), texture_name)) };

		if (!res::get_system().memory_resolver_.is_exist(desc_tag)) {
			json::object desc;
			desc["__parent"] = "res://base_texture.desc";
			desc["name"] = texture_name;
			desc["data"] = json::value_from(tag + res::tag::make(texture_name));

			std::string data = json::serialize(desc);
			std::vector<std::byte> desc_data(data.size());
			std::memcpy(desc_data.data(), data.data(), data.size());
			res::get_system().memory_resolver_.add_memory_resource(desc_tag, desc_data);
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

ds::svg_writer writer("mesh_bounds.svg", 2048, 2048);

void process_mesh(const aiScene* scene, const aiMesh* mesh, json::object& jsmesh, rnd::geometry_desc& geometry, res::tag tag)
{
	glm::ivec2 vertex_range{ geometry.vertices.size() / geometry.layout.get_stride(), 0 };
	glm::ivec2 index_range{ geometry.indices.size(), 0 };

	std::size_t weight_offset = 0;

	for (unsigned int i = 0; i < mesh->mNumVertices; i++)
	{
		size_t stride = sizeof(glm::vec3);
		size_t begin = geometry.vertices.size();

		store_data(geometry.vertices, convert_to_glm(mesh->mVertices[i]));
		// normals
		if (mesh->HasNormals())
		{
			stride += sizeof(glm::vec3);
			store_data(geometry.vertices, convert_to_glm(mesh->mNormals[i]));
		}
		// texture coordinates
		if (mesh->HasTextureCoords(0))
		{
			stride += sizeof(glm::vec2);
			store_data(geometry.vertices, (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][i]));
		}

		if (mesh->HasTangentsAndBitangents())
		{
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
		// retrieve all indices of the face and store them in the indices vector
		for (unsigned int j = 0; j < face.mNumIndices; j++) {
			geometry.indices.push_back(face.mIndices[j]);
		}

		for (unsigned int j = 0; j < face.mNumIndices; j += 3) {
			struct bbox1
			{
				struct { glm::vec2 min; glm::vec2 max; };

				bbox1()
					: min{ std::numeric_limits<float>::max() }
					, max{ std::numeric_limits<float>::lowest() }
				{
				}

				void expand(const glm::vec2& point)
				{
					min = glm::min(min, point);
					max = glm::max(max, point);
				}
			};

			auto uv1 = (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][face.mIndices[j]]);
			auto uv2 = (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][face.mIndices[j + 1]]);
			auto uv3 = (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][face.mIndices[j + 2]]);

			ds::bbox box;
			ds::expand(box, uv1);
			ds::expand(box, uv2);
			ds::expand(box, uv3);

			writer.add_rect(box);

			geometry.bounds.push_back({ box, geometry.bounds.size() });
		}
	}

	vertex_range.y = geometry.vertices.size() / geometry.layout.get_stride();
	index_range.y = geometry.indices.size();

	jsmesh["mesh"] = json::object{
		{"vx_begin", vertex_range.x},
		{"vx_end", vertex_range.y},
		{"ind_begin", index_range.x},
		{"ind_end", index_range.y},
		{"material", process_material(scene, scene->mMaterials[mesh->mMaterialIndex], tag)}
	};

	json::array weights_to_vertex;
	weights_to_vertex.resize(mesh->mNumVertices);

	for (unsigned int bidx = 0; bidx < mesh->mNumBones; ++bidx)
	{
		aiBone* b = mesh->mBones[bidx];

		for (unsigned int widx = 0; widx < b->mNumWeights; ++widx)
		{
			const auto& [vertex_idx, weight] = b->mWeights[widx];
			std::size_t cur_idx = (std::size_t)vertex_range.x + vertex_idx;
			json::object jsboneweight;
			jsboneweight["bone_name"] = b->mName.C_Str();
			jsboneweight["weight"] = weight;

			auto& v = weights_to_vertex.at(vertex_idx);

			if (auto* varr = v.if_array()) {
				varr->push_back(jsboneweight);
			} else {
				v = json::array{ jsboneweight };
			}
		}
	}

	if (auto* jm = jsmesh.if_contains("mesh")) {
		jm->as_object()["weights"] = weights_to_vertex;
	}
}

void process_node(const aiScene* scene, aiNode* node, json::object& jsnode, rnd::geometry_desc& geometry, res::tag tag)
{
	jsnode["name"] = node->mName.C_Str();
	jsnode["local"] = json::value_from(convert_to_glm(node->mTransformation));

	json::array children;
	for (unsigned int i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		json::object jsmesh;
		jsmesh["name"] = "mesh"s + mesh->mName.C_Str();
		process_mesh(scene, mesh, jsmesh, geometry, tag);
		children.push_back(jsmesh);
	}

	if (auto* bone = scene->findBone(node->mName))
	{
		jsnode["bone"] = json::object
		{
			{"name", bone->mName.C_Str()},
			{"offset_matrix", json::value_from(convert_to_glm(bone->mOffsetMatrix))}
		};
	}

	for (unsigned int i = 0; i < node->mNumChildren; i++)
	{
		json::object child;
		process_node(scene, node->mChildren[i], child, geometry, tag);
		children.push_back(child);
	}

	if (!children.empty()) {
		jsnode["children"] = children;
	}
}

void process_animations(const aiScene* scene, json::object& jsanimations, res::tag tag)
{
	for (unsigned int i = 0; i < scene->mNumAnimations; i++) {
		auto animation = scene->mAnimations[i];
		json::object jsanimation;
		jsanimation["name"] = animation->mName.C_Str();
		jsanimation["duration"] = animation->mDuration;
		jsanimation["ticks_per_second"] = animation->mTicksPerSecond;
		/*"keyframes": [
			"node_name":{
				"position": [
				{ "value": [0, 0, 0] , "time" : 0.0 },
				{ "value": [1, 1, 1] , "time" : 1.0 }
				] ,
					"rotation" : [
				{ "value": [0, 0, 0, 1] , "time" : 0.0 },
				{ "value": [1, 0, 0, 0] , "time" : 1.0 }
					] ,
					"scale" : [
				{ "value": [1, 1, 1] , "time" : 0.0 },
				{ "value": [2, 2, 2] , "time" : 1.0 }
					]
			}
		]*/
		json::object keyframes;

		for (std::size_t idx = 0; idx < animation->mNumChannels; ++idx) {
			auto* pAnimNode = animation->mChannels[idx];
			std::string_view node_name = pAnimNode->mNodeName.C_Str();
			json::object node;
			node["name"] = node_name;

			json::array position_keys;
			for (std::size_t idx = 0; idx < pAnimNode->mNumPositionKeys; ++idx)
			{
				position_keys.push_back(
					{
						{"value", json::value_from(convert_to_glm(pAnimNode->mPositionKeys[idx].mValue))},
						{"time", pAnimNode->mPositionKeys[idx].mTime}
					}
				);
			}

			node["position"] = position_keys;

			json::array rotation_keys;
			for (std::size_t idx = 0; idx < pAnimNode->mNumRotationKeys; ++idx)
			{
				rotation_keys.push_back(
					{
						{"value", json::value_from(convert_to_glm(pAnimNode->mRotationKeys[idx].mValue))},
						{"time", pAnimNode->mRotationKeys[idx].mTime}
					}
				);
			}

			node["rotation"] = rotation_keys;

			json::array scale_keys;
			for (std::size_t idx = 0; idx < pAnimNode->mNumScalingKeys; ++idx)
			{
				scale_keys.push_back(
					{
						{"value", json::value_from(convert_to_glm(pAnimNode->mScalingKeys[idx].mValue))},
						{"time", pAnimNode->mScalingKeys[idx].mTime}
					}
				);
			}

			node["scale"] = scale_keys;
		
			keyframes[node_name] = node;
		}

		jsanimation["keyframes"] = keyframes;

		jsanimations[animation->mName.C_Str()] = jsanimation;
	}
}

void process_model(const aiScene* scene, json::object& data, res::tag tag)
{
    json::object jsgeometry;
    json::object jstree;
    jsgeometry["__parent"] = "res://base_geometry.desc";
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
		if (mesh->HasBones()) {
			//layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC4_F, "bones_weight" });
		}
		if (mesh->HasVertexColors(0)) {
			//layout.push_back({ rnd::driver::SHADER_DATA_TYPE::VEC4_F, "color" });
		}
		geometry.layout = rnd::driver::BufferLayout{ layout };
	}

	process_node(scene, scene->mRootNode, jstree, geometry, tag);
	geometry.serialize(jsgeometry);
    data["geometry"] = jsgeometry;
    data["tree"] = jstree;

	if (scene->HasAnimations()) {
		json::object jsanimations;
		process_animations(scene, jsanimations, tag);
		data["animations"] = jsanimations;
	}
}