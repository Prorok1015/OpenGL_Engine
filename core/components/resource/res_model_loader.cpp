#include "res_model_loader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <common.h>
#include <engine_log.h>
#include "res_system.h"
#include "adapters/res_pct_adapter.h"

const aiNodeAnim* find_node_anim(const aiAnimation* pAnimation, const std::string_view NodeName);

glm::vec4 get_material_color(aiMaterial* material, const char* pKey, unsigned int type, unsigned int idx, const glm::vec4& defaultValue) {
    aiColor4D color;
    if (AI_SUCCESS == material->Get(pKey, type, idx, color)) {
        return glm::vec4(color.r, color.g, color.b, color.a);
    } else {
        egLOG("loader", "Material color not found. Using default color.");
        return defaultValue;
    }
}

std::string get_material_string(aiMaterial* material, const char* pKey, unsigned int type, unsigned int idx, const std::string& defaultValue) {
    aiString str;
    if (AI_SUCCESS == material->Get(pKey, type, idx, str)) {
        return { str.C_Str() };
    } else {
        egLOG("loader", "Material color not found. Using default color.");
        return defaultValue;
    }
}

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

glm::mat4 convert_to_glm(const aiMatrix4x4& transform) {
    glm::mat4 local(1);
    local[0] = glm::vec4(transform.a1, transform.b1, transform.c1, transform.d1);
    local[1] = glm::vec4(transform.a2, transform.b2, transform.c2, transform.d2);
    local[2] = glm::vec4(transform.a3, transform.b3, transform.c3, transform.d3);
    local[3] = glm::vec4(transform.a4, transform.b4, transform.c4, transform.d4);
    return local;
}

bool res::Vertex::operator== (const Vertex& rhs) const
{
    return position == rhs.position &&
        normal == rhs.normal &&
        uv == rhs.uv &&
        tangent == rhs.tangent &&
        bitangent == rhs.bitangent &&
        color == rhs.color &&
        bones_weight == rhs.bones_weight
        ;
}

void res::loader::model_loader::trace_node(const std::stack<aiNode*>& stack)
{
    if (stack.empty()) {
        return;
    }

    std::string node = stack.top()->mName.C_Str();
    std::string space(stack.size() - 1, '-');

    egLOG("test", space + node);
}

std::vector<res::Mesh> res::loader::model_loader::load()
{
    // read file via ASSIMP
    Assimp::Importer importer;
    const std::string full_path = resource_system::get_absolut_path(tag);
    egLOG("model/load", "Start loading model '{}' by absolut path:\n\t '{}'", tag.get_full(), full_path);

    const aiScene* scene = importer.ReadFile(full_path, aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);
    // check for errors
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) // if is Not Zero
    {
        egLOG("scene/model/load", "ERROR::ASSIMP::{}", importer.GetErrorString());
        return {};
    }


    /* .material
    {
		name: string,
		diffuse: vec4,
		ambient: vec4,
		specular: vec4,
		shininess: float,
		normal: tag,
		specular: tag,
		diffuse: tag,
		ambient: tag,
    }
    */
    // process materials
    for (int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial* material = scene->mMaterials[i];
        res::Material mesh_material = copy_material(scene, material);
    }

    aiNode* Root = scene->mRootNode;
    // process ASSIMP's root node recursively
    process_node(Root, scene, model.head);

    for (std::size_t anim_idx = 0; anim_idx < scene->mNumAnimations; ++anim_idx) {
        auto* pAnimation = scene->mAnimations[anim_idx];
        res::animation anim;
        anim.name = pAnimation->mName.C_Str();
        anim.duration = pAnimation->mDuration;
        anim.ticks_per_second = pAnimation->mTicksPerSecond;

        model.animations.push_back(anim);

        for (std::size_t i = 0; i < pAnimation->mNumChannels; i++) {
            const aiNodeAnim* pNodeAnim = pAnimation->mChannels[i];

            dbgAnimatedNodes.push_back(pNodeAnim->mNodeName.C_Str());
        }
    }

    enclose_hierarchy(model.head, scene);
    /* .geometry
    {
		pos:       vec3,
        normal:    vec3,
		uv:        vec2,
		tangent:   vec3,
		bitangent: vec3,
    }
    {   res::Vertex[]  }
    {   uint[]  }
    {   uint[]  }
    */
    model.data.bones_data.bones_indeces.resize(model.data.vertices.size() * (max_bones_count), -1);
    for (std::size_t v_idx = 0, offset = 0; v_idx < model.data.vertices.size(); )
    {
        auto& bones_indeces = vertex_bone_mapping[v_idx];
        ASSERT_MSG(bones_indeces.size() <= max_bones_count, "Size > max_size");

        for (int b_idx = 0; b_idx < bones_indeces.size(); ++b_idx)
        {
            std::size_t offset_b = b_idx * model.data.vertices.size();
            model.data.bones_data.bones_indeces[offset_b + v_idx] = bones_indeces[b_idx];
        }
        ++v_idx;
        offset = v_idx * max_bones_count;
    }
    model.data.bones_matrices.reserve(model.data.bones.size());


    egLOG("load/deep", "max deep: {}", deep);
    egLOG("load/meshes", "meshes: {}", meshes.size());
    for (auto& [txt, count] : textures_counter)
    {
        egLOG("load/textures", "texture: {}, use: {}", txt.get_full(), count);
    }

    for (auto& [mesh, count] : meshes_counter)
    {
        egLOG("load/meshes", "mesh: {}, use: {}", (void*)mesh, count);
    }

    for (auto& mesh : meshes)
    {
        //egLOG("load/meshes", "mesh: vx - {}   \tind - {}   \tbouns - {},   \tmaterial: {}, {}, {}, {}", mesh.vertices.size(), mesh.indices.size(), mesh.bones.size(),
        //    mesh.material.diffuse.name(), mesh.material.ambient.name(), mesh.material.normal.name(), mesh.material.specular.name());
    }

    for (auto& mesh : model.meshes_views)
    {
        egLOG("load/meshes", "mesh: vx - {}   \tind - {}   \tbouns - {}", mesh.get_vertices_count(), mesh.get_indices_count(), mesh.get_bones_count());
    }


    //for (std::size_t idx = 0; idx < meshes.size(); ++idx)
    //{
    //    auto& mesh = meshes[idx];
    //    auto& mesh_view = model.meshes_views[idx];
    //    for (std::size_t vert_idx = 0; vert_idx < mesh.vertices.size(); ++vert_idx)
    //    {
    //        auto& vertex = mesh.vertices[vert_idx];
    //        auto& vertex_view = model.data.vertices[mesh_view.vx_begin + vert_idx];
    //        if (vertex != vertex_view) {
    //            egLOG("load/check", "vx {} != {}", vert_idx, mesh_view.vx_begin + vert_idx);
    //        }
    //    }
    //}

    //batch_meshes();
    return meshes;
}

void res::loader::model_loader::process_node(aiNode* node, const aiScene* scene, res::node_hierarchy_view& hierarchy)
{
    transform_stack.push(convert_to_glm(node->mTransformation));
    nodes.push(node);
    deep = std::max(deep, transform_stack.size());
    trace_node(nodes);

    hierarchy.children.push_back(res::node_hierarchy_view{});
    res::node_hierarchy_view& current = hierarchy.children.back();
    current.name = node->mName.C_Str();
    current.mt = convert_to_glm(node->mTransformation);

    // process each mesh located at the current node
    for (unsigned int i = 0; i < node->mNumMeshes; i++)
    {
        // the node object only contains indices to index the actual objects in the scene. 
        // the scene contains all the data, node is just to keep stuff organized (like relations between nodes).
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];

        meshes.push_back(copy_mesh(mesh, scene, current));
        current.meshes.push_back(model.meshes_views.back());
    }

    // after we've processed all of the meshes (if any) we then recursively process each of the children nodes
    for (unsigned int i = 0; i < node->mNumChildren; i++)
    {
        process_node(node->mChildren[i], scene, current);
    }


    nodes.pop();
    transform_stack.pop();
}

res::Mesh res::loader::model_loader::copy_mesh(aiMesh* mesh, const aiScene* scene, res::node_hierarchy_view& hierarchy)
{
    std::size_t ind_offset = model.data.indices.size();
    std::size_t verx_offset = model.data.vertices.size();
    std::size_t bone_offset = model.data.bones.size();

    std::vector<res::Vertex> vertices = copy_vertices(mesh);

    std::vector<res::bone> bones = copy_bones(vertices, mesh, verx_offset);

    std::vector<unsigned int> indices = copy_indeces(mesh);

    res::mesh_view v_mesh;
    v_mesh.ind_begin = ind_offset;
    v_mesh.ind_end = model.data.indices.size();
    v_mesh.vx_begin = verx_offset;
    v_mesh.vx_end = model.data.vertices.size();
    v_mesh.bones_begin = bone_offset;
    v_mesh.bones_end = model.data.bones.size();
    v_mesh.material_id = mesh->mMaterialIndex;

    model.meshes_views.push_back(v_mesh);

    meshes_counter[mesh]++;
    // return a mesh object created from the extracted mesh data
    return res::Mesh{ .vertices = vertices, .indices = indices, .bones = bones };
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

res::Material res::loader::model_loader::copy_material(const aiScene* scene, aiMaterial* material)
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

    res::Material mesh_material;
    // 1. diffuse maps
    auto tmp = find_material_texture(scene, material, aiTextureType_BASE_COLOR);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::ALBEDO_TXM] = tmp;
        mesh_material.set_state(res::Material::ALBEDO_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_DIFFUSE);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::DIFFUSE_TXM] = tmp;
        mesh_material.set_state(res::Material::DIFFUSE_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_SPECULAR);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::SPECULAR_TXM] = tmp;
        mesh_material.set_state(res::Material::SPECULAR_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_HEIGHT);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::HEIGHT_TXM] = tmp;
        mesh_material.set_state(res::Material::HEIGHT_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_NORMALS);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::NORMALS_TXM] = tmp;
        mesh_material.set_state(res::Material::NORMALS_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_EMISSIVE);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::EMISSIVE_TXM] = tmp;
        mesh_material.set_state(res::Material::EMISSIVE_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_CLEARCOAT);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::CLEARCOAT_TXM] = tmp;
        mesh_material.set_state(res::Material::CLEARCOAT_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_SHININESS);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::SHININESS_TXM] = tmp;
        mesh_material.set_state(res::Material::SHININESS_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_OPACITY);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::OPACITY_TXM] = tmp;
        mesh_material.set_state(res::Material::OPACITY_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_LIGHTMAP);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::LIGHTMAP_TXM] = tmp;
        mesh_material.set_state(res::Material::LIGHTMAP_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_EMISSION_COLOR);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::EMISSION_COLOR_TXM] = tmp;
        mesh_material.set_state(res::Material::EMISSION_COLOR_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_METALNESS);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::METALNESS_TXM] = tmp;
        mesh_material.set_state(res::Material::METALNESS_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_DIFFUSE_ROUGHNESS);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::DIFFUSE_ROUGHNESS_TXM] = tmp;
        mesh_material.set_state(res::Material::DIFFUSE_ROUGHNESS_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_TRANSMISSION);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::TRANSMISSION_TXM] = tmp;
        mesh_material.set_state(res::Material::TRANSMISSION_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_AMBIENT_OCCLUSION);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::AMBIENT_OCCLUSION_TXM] = tmp;
        mesh_material.set_state(res::Material::AMBIENT_OCCLUSION_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_SHEEN);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::SHEEN_TXM] = tmp;
        mesh_material.set_state(res::Material::SHEEN_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_REFLECTION);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::REFLECTION_TXM] = tmp;
        mesh_material.set_state(res::Material::REFLECTION_TXM);
        increase_txt_counter(tmp);
    }

    tmp = find_material_texture(scene, material, aiTextureType_AMBIENT);
    if (tmp.is_valid()) {
        mesh_material.txm_list[res::Material::AMBIENT_TXM] = tmp;
        mesh_material.set_state(res::Material::AMBIENT_TXM);
        increase_txt_counter(tmp);
    }

    mesh_material.name = get_material_string(material, AI_MATKEY_NAME, "new_material");
    aiColor4D color;
    if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color)) {
        mesh_material.diffuse_color = glm::vec4(color.r, color.g, color.b, color.a);
        mesh_material.set_state(res::Material::ALBEDO_COLOR);
    }
    if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_SPECULAR, color)) {
        mesh_material.specular_color = glm::vec4(color.r, color.g, color.b, color.a);
        mesh_material.set_state(res::Material::SPECULAR_COLOR);
    }
    if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_AMBIENT, color)) {
        mesh_material.ambient_color = glm::vec4(color.r, color.g, color.b, color.a);
        mesh_material.set_state(res::Material::AMBIENT_COLOR);
    }
    if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_EMISSIVE, color)) {
        mesh_material.emissive_color = glm::vec4(color.r, color.g, color.b, color.a);
        mesh_material.set_state(res::Material::EMISSIVE_COLOR);
    }
    
    if (AI_SUCCESS == material->Get(AI_MATKEY_OPACITY, mesh_material.opacity)) {
        mesh_material.set_state(res::Material::OPACITY);
    }
    if (AI_SUCCESS == material->Get(AI_MATKEY_REFLECTIVITY, mesh_material.reflectivity)) {
        mesh_material.set_state(res::Material::REFLECTIVITY);
    }
    if (AI_SUCCESS == material->Get(AI_MATKEY_REFRACTI, mesh_material.refracti)) {
        mesh_material.set_state(res::Material::REFRACTI);
    }
    if (AI_SUCCESS == material->Get(AI_MATKEY_SHININESS, mesh_material.shininess)) {
        mesh_material.set_state(res::Material::SHININESS);
    }
    if (AI_SUCCESS == material->Get(AI_MATKEY_SHININESS_STRENGTH, mesh_material.shininess_strength)) {
        mesh_material.set_state(res::Material::SHININESS_STRENGTH);
    }
    if (AI_SUCCESS == material->Get(AI_MATKEY_BUMPSCALING, mesh_material.bump_scaling)) {
        mesh_material.set_state(res::Material::BUMP_SCALING);
    }
    model.data.materials.push_back(mesh_material);
    return mesh_material;
}

std::vector<res::Vertex> res::loader::model_loader::copy_vertices(aiMesh* mesh)
{
    std::vector<Vertex> vertices;
    vertices.reserve(mesh->mNumVertices);

    model.data.vertices.reserve(model.data.vertices.size() + mesh->mNumVertices);
    int normals_count = 0;
    int uv_count = 0;
    int tangents_count = 0;
    int bitangents_count = 0;
    int color_count = 0;

    // walk through each of the mesh's vertices
    for (unsigned int i = 0; i < mesh->mNumVertices; i++)
    {
        Vertex vertex;
        glm::vec3 vector; // we declare a placeholder vector since assimp uses its own vector class that doesn't directly convert to glm's vec3 class so we transfer the data to this placeholder glm::vec3 first.
        // positions
        vector.x = mesh->mVertices[i].x;
        vector.y = mesh->mVertices[i].y;
        vector.z = mesh->mVertices[i].z;

        glm::mat4 matrix = glm::translate(vector);

        //for (auto trans = transform_stack; !trans.empty(); trans.pop())
        //{
        //    matrix = trans.top() * matrix;
        //}

        glm::vec3 s;
        glm::quat o;
        glm::vec3 sk;
        glm::vec4 p;
        glm::decompose(matrix, s, o, vertex.position, sk, p);

        // normals
        if (mesh->HasNormals())
        {
            vector.x = mesh->mNormals[i].x;
            vector.y = mesh->mNormals[i].y;
            vector.z = mesh->mNormals[i].z;
            vertex.normal = vector;
            normals_count++;
        }

        // texture coordinates
        if (mesh->HasTextureCoords(0)) // does the mesh contain texture coordinates?
        {
            glm::vec2 vec;
            // a vertex can contain up to 8 different texture coordinates. We thus make the assumption that we won't 
            // use models where a vertex can have multiple texture coordinates so we always take the first set (0).
            vec.x = mesh->mTextureCoords[0][i].x;
            vec.y = mesh->mTextureCoords[0][i].y;
            vertex.uv = vec;
            uv_count++;
        }

        if (mesh->HasTangentsAndBitangents())
        {
            // tangent
            vector.x = mesh->mTangents[i].x;
            vector.y = mesh->mTangents[i].y;
            vector.z = mesh->mTangents[i].z;
            vertex.tangent = vector;
            tangents_count++;
            // bitangent

            vector.x = mesh->mBitangents[i].x;
            vector.y = mesh->mBitangents[i].y;
            vector.z = mesh->mBitangents[i].z;
            vertex.bitangent = vector;
            bitangents_count++;
        }

        if (mesh->HasVertexColors(i))
        {
            glm::vec4 color;
            color.r = mesh->mColors[i]->r;
            color.g = mesh->mColors[i]->g;
            color.b = mesh->mColors[i]->b;
            color.a = mesh->mColors[i]->a;
            vertex.color = color;
            color_count++;
        }
        vertices.push_back(vertex);
        model.data.vertices.push_back(vertex);
    }

    egLOG("loader", "Vertex count: {0}, Normals: {1}, UVs: {2}, Tangents: {3}, Bitangents: {4}, Colors: {5}",
        mesh->mNumVertices,
        normals_count,
        uv_count,
        tangents_count,
        bitangents_count,
        color_count
        );

    return vertices;
}

std::vector<res::bone> res::loader::model_loader::copy_bones(std::vector<res::Vertex>& vertices, aiMesh* mesh, std::size_t base_vertex)
{
    std::vector<res::bone> bones(mesh->mNumBones);
    model.data.bones.reserve(model.data.bones.size() + mesh->mNumBones);

    for (unsigned int i = 0; i < mesh->mNumBones; i++) {
        aiBone* pBone = mesh->mBones[i];

        int bone_id = 0;
        std::string name = pBone->mName.C_Str();

        if (auto it = bones_mapping.find(name); it != bones_mapping.end()) {
            bone_id = it->second;
        } else {
            bone_id = model.data.bones.size();
            bones_mapping[name] = bone_id;
            model.data.bones.push_back(res::bone{ .offset = convert_to_glm(pBone->mOffsetMatrix) });
        }

        for (unsigned int i = 0; i < pBone->mNumWeights; i++) {
            const auto& [vertex_id, weight] = pBone->mWeights[i];

            auto& vertex = model.data.vertices[base_vertex + vertex_id];     
            if (weight > 0.0) {
                auto& bones_indeces = vertex_bone_mapping[base_vertex + vertex_id];
                std::size_t idx = bones_indeces.size();
                if (idx < vertex.bones_weight.length()) {
                    bones_indeces.push_back(bone_id);
                    vertex.bones_weight[idx] = weight;
                    max_bones_count = std::max(max_bones_count, idx + 1);
                }
            }
        }
    }

    return bones;
}

std::vector<unsigned int> res::loader::model_loader::copy_indeces(aiMesh* mesh)
{
    std::vector<unsigned int> indices;
    indices.reserve(mesh->mNumFaces);

    model.data.indices.reserve(model.data.indices.size() + mesh->mNumFaces * 3);
    // now wak through each of the mesh's faces (a face is a mesh its triangle) and retrieve the corresponding vertex indices.
    for (unsigned int i = 0; i < mesh->mNumFaces; i++)
    {
        aiFace face = mesh->mFaces[i];
        // retrieve all indices of the face and store them in the indices vector
        for (unsigned int j = 0; j < face.mNumIndices; j++) {
            indices.push_back(face.mIndices[j]);
            model.data.indices.push_back(face.mIndices[j]);
        }
    }

    return indices;
}

res::tag res::loader::model_loader::find_material_texture(const aiScene* scene, aiMaterial* mat, aiTextureType type) {
    for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
    {
        aiString str;
        mat->GetTexture(type, i, &str);
        std::string_view texture_name = str.C_Str();
        if (auto* pEmbededTxm = scene->GetEmbeddedTexture(texture_name.data()))
        {
            const std::string_view embedded_filename{ pEmbededTxm->mFilename.C_Str() };
            const std::string_view embedded_format{ pEmbededTxm->achFormatHint };
            std::string embedded_path = std::vformat("__embedded_txm_{0}/{1}.{2}", std::make_format_args(tag.pure_name(), embedded_filename, 
                pEmbededTxm->mHeight != 0 ? res::raw_image_adapter::EXTENSION : res::pct_adapter::EXTENSIONS[0]));
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

            return embedded_tag;
        }
        return tag + tag::make(texture_name);
    }
    return {};
}

std::optional<res::animation_node> parse_key_frames(aiAnimation* pAnimation, std::string_view node_name)
{
    std::optional<res::animation_node> resalt;
    if (auto* pAnimNode = find_node_anim(pAnimation, node_name))
    {
        res::animation_node anim;
        anim.pos_keys.reserve(pAnimNode->mNumPositionKeys);
        anim.rotate_keys.reserve(pAnimNode->mNumRotationKeys);
        anim.scale_keys.reserve(pAnimNode->mNumScalingKeys);

        for (std::size_t idx = 0; idx < pAnimNode->mNumPositionKeys; ++idx)
        {
            res::animation_pos_key key;
            key.value = convert_to_glm(pAnimNode->mPositionKeys[idx].mValue);
            key.time = pAnimNode->mPositionKeys[idx].mTime;
            anim.pos_keys.push_back(key);
        }

        for (std::size_t idx = 0; idx < pAnimNode->mNumRotationKeys; ++idx)
        {
            res::animation_rotate_key key;
            key.value = convert_to_glm(pAnimNode->mRotationKeys[idx].mValue);
            key.time = pAnimNode->mRotationKeys[idx].mTime;
            anim.rotate_keys.push_back(key);
        }

        for (std::size_t idx = 0; idx < pAnimNode->mNumScalingKeys; ++idx)
        {
            res::animation_scale_key key;
            key.value = convert_to_glm(pAnimNode->mScalingKeys[idx].mValue);
            key.time = pAnimNode->mScalingKeys[idx].mTime;
            anim.scale_keys.push_back(key);
        }

        resalt.emplace(std::move(anim));
    }

    return resalt;
}

void res::loader::model_loader::enclose_hierarchy(res::node_hierarchy_view& node, const aiScene* scene)
{
    if (auto it = bones_mapping.find(node.name); it != bones_mapping.end()) {
        node.bone_id = it->second;
        for (std::size_t anim_idx = 0; anim_idx < scene->mNumAnimations; ++anim_idx) {
            aiAnimation* pAnimation = scene->mAnimations[anim_idx];
            std::string animation_name = pAnimation->mName.C_Str();
            auto anim = parse_key_frames(pAnimation, node.name);
            if (anim.has_value()) {
                model.data.bones[node.bone_id].anim[animation_name] = anim.value();
            }
        }
    }
    else {
        for (std::size_t anim_idx = 0; anim_idx < scene->mNumAnimations; ++anim_idx) {
            aiAnimation* pAnimation = scene->mAnimations[anim_idx];
            std::string animation_name = pAnimation->mName.C_Str();
            auto anim = parse_key_frames(pAnimation, node.name);
            if (anim.has_value()) {
                node.anim[animation_name] = anim.value();
            }
        }
    }

    for (auto& child : node.children)
    {
        enclose_hierarchy(child, scene);
    }
}

const aiNodeAnim* find_node_anim(const aiAnimation* pAnimation, const std::string_view NodeName)
{
    for (std::size_t i = 0; i < pAnimation->mNumChannels; i++) {
        const aiNodeAnim* pNodeAnim = pAnimation->mChannels[i];

        if (std::string_view(pNodeAnim->mNodeName.C_Str()) == NodeName) {
            return pNodeAnim;
        }
    }

    return NULL;
}

void res::loader::model_loader::batch_meshes()
{
    //std::unordered_map<std::size_t, std::vector<std::size_t>> batches;
    //bool is_need_batch = false;
    //for (std::size_t idx = 0; idx < meshes.size(); ++idx)
    //{
    //    auto& mesh = meshes[idx];
    //    std::size_t batch_idx = mesh.material.diffuse.get_hash()
    //        | mesh.material.ambient.get_hash()
    //        | mesh.material.normal.get_hash()
    //        | mesh.material.specular.get_hash();
    //    batches[batch_idx].push_back(idx);
    //}

    //std::vector<res::Mesh> batch_result;
    //for (auto& [_, bmeshes] : batches)
    //{
    //    res::Mesh new_mesh = meshes[bmeshes[0]];
    //    egLOG("load/batch", "batch {}", new_mesh.material.diffuse.get_full());

    //    for (std::size_t idx = 1; idx < bmeshes.size(); ++idx)
    //    {
    //        auto& mesh = meshes[bmeshes[idx]];

    //        std::size_t addition = new_mesh.vertices.size();

    //        new_mesh.vertices.reserve(new_mesh.vertices.size() + mesh.vertices.size());
    //        std::copy(mesh.vertices.begin(), mesh.vertices.end(), std::back_inserter(new_mesh.vertices));

    //        std::for_each(mesh.indices.begin(), mesh.indices.end(), [addition](unsigned int& inc) { inc += addition; });

    //        new_mesh.indices.reserve(new_mesh.indices.size() + mesh.indices.size());
    //        std::copy(mesh.indices.begin(), mesh.indices.end(), std::back_inserter(new_mesh.indices));
    //    }
    //    batch_result.push_back(new_mesh);
    //}

    //if (!batch_result.empty()) {
    //    egLOG("load/batching", "from {} meshes convert to {} meshes", meshes.size(), batch_result.size());

    //    meshes = batch_result;
    //}
}
