#include "res_model_loader.h"
#include "res_model_loader.h"
#include "res_model_loader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "res_model_loader.h"
#include <common.h>
#include <engine_log.h>
#include <res_resource_system.h>


bool res::Vertex::operator== (const Vertex& rhs) const
{
    return position == rhs.position &&
        normal == rhs.normal &&
        uv == rhs.uv &&
        tangent == rhs.tangent &&
        bitangent == rhs.bitangent &&
        bones == rhs.bones &&
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
    const std::string full_path = ResourceSystem::get_absolut_path(tag);
    egLOG("model/load", "Start loading model '{}' by absolut path:\n\t '{}'", tag.get_full(), full_path);

    const aiScene* scene = importer.ReadFile(full_path, aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);
    // check for errors
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) // if is Not Zero
    {
        egLOG("scene/model/load", "ERROR::ASSIMP::{}", importer.GetErrorString());
        return {};
    }

    aiNode* Root = scene->mRootNode;
    if (tag.name().find(".gltf") != std::string::npos) {
        for (int i = 0; i < 2; ++i) {
            if (Root->mNumChildren > 0) {
                Root = Root->mChildren[0];
            }
        }
    }

    // process ASSIMP's root node recursively
    process_node(Root, scene, model.head);

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
        egLOG("load/meshes", "mesh: vx - {}   \tind - {}   \tbouns - {},   \tmaterial: {}, {}, {}, {}", mesh.vertices.size(), mesh.indices.size(), mesh.bones.size(),
            mesh.material.diffuse.name(), mesh.material.ambient.name(), mesh.material.normal.name(), mesh.material.specular.name());
    }

    for (auto& mesh : model.meshes_views)
    {
        egLOG("load/meshes", "mesh: vx - {}   \tind - {}   \tbouns - {}", mesh.get_vertices_count(), mesh.get_indices_count(), mesh.get_bones_count());
    }


    for (std::size_t idx = 0; idx < meshes.size(); ++idx)
    {
        auto& mesh = meshes[idx];
        auto& mesh_view = model.meshes_views[idx];
        for (std::size_t vert_idx = 0; vert_idx < mesh.vertices.size(); ++vert_idx)
        {
            auto& vertex = mesh.vertices[vert_idx];
            auto& vertex_view = model.data.vertices[mesh_view.vx_begin + vert_idx];
            if (vertex != vertex_view) {
                egLOG("load/check", "vx {} != {}", vert_idx, mesh_view.vx_begin + vert_idx);
            }

        }

    }

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
    std::size_t mat_offset = model.data.materials.size();

    std::vector<res::Vertex> vertices = copy_vertices(mesh);

    std::vector<res::bone> bones = copy_bones(vertices, mesh, bone_offset);

    std::vector<unsigned int> indices = copy_indeces(mesh);

    // process materials
    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
    // we assume a convention for sampler names in the shaders. Each diffuse texture should be named
    // as 'texture_diffuseN' where N is a sequential number ranging from 1 to MAX_SAMPLER_NUMBER. 
    // Same applies to other texture as the following list summarizes:
    // diffuse: texture_diffuseN
    // specular: texture_specularN
    // normal: texture_normalN

    res::Material mesh_material = copy_material(material);

    res::mesh_view v_mesh;
    v_mesh.ind_begin = ind_offset;
    v_mesh.ind_end = model.data.indices.size();
    v_mesh.vx_begin = verx_offset;
    v_mesh.vx_end = model.data.vertices.size();
    v_mesh.bones_begin = bone_offset;
    v_mesh.bones_end = model.data.bones.size();
    v_mesh.material_id = mat_offset;

    model.meshes_views.push_back(v_mesh);

    meshes_counter[mesh]++;
    // return a mesh object created from the extracted mesh data
    return res::Mesh{ .vertices = vertices, .indices = indices, .bones = bones, .material = mesh_material };
}

res::Material res::loader::model_loader::copy_material(aiMaterial* material)
{
    res::Material mesh_material;
    // 1. diffuse maps
    mesh_material.diffuse = find_material_texture(material, aiTextureType_DIFFUSE);
    increase_txt_counter(mesh_material.diffuse);
    // 2. specular maps
    mesh_material.specular = find_material_texture(material, aiTextureType_SPECULAR);
    increase_txt_counter(mesh_material.specular);
    // 3. normal maps
    mesh_material.normal = find_material_texture(material, aiTextureType_HEIGHT);
    increase_txt_counter(mesh_material.normal);
    // 4. ambient maps
    mesh_material.ambient = find_material_texture(material, aiTextureType_AMBIENT);
    increase_txt_counter(mesh_material.ambient);

    model.data.materials.push_back(mesh_material);

    return mesh_material;
}

std::vector<res::Vertex> res::loader::model_loader::copy_vertices(aiMesh* mesh)
{
    std::vector<Vertex> vertices;
    vertices.reserve(mesh->mNumVertices);

    model.data.vertices.reserve(model.data.vertices.size() + mesh->mNumVertices);

    // walk through each of the mesh's vertices
    for (unsigned int i = 0; i < mesh->mNumVertices; i++)
    {
        res::Vertex native_vertex;
        Vertex vertex;
        glm::vec3 vector; // we declare a placeholder vector since assimp uses its own vector class that doesn't directly convert to glm's vec3 class so we transfer the data to this placeholder glm::vec3 first.
        // positions
        vector.x = mesh->mVertices[i].x;
        vector.y = mesh->mVertices[i].y;
        vector.z = mesh->mVertices[i].z;

        native_vertex.position = vector;

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
            native_vertex.normal = vector;
        }

        // texture coordinates
        if (mesh->mTextureCoords[0]) // does the mesh contain texture coordinates?
        {
            glm::vec2 vec;
            // a vertex can contain up to 8 different texture coordinates. We thus make the assumption that we won't 
            // use models where a vertex can have multiple texture coordinates so we always take the first set (0).
            vec.x = mesh->mTextureCoords[0][i].x;
            vec.y = mesh->mTextureCoords[0][i].y;
            vertex.uv = vec;
            native_vertex.uv = vec;
            // tangent
            vector.x = mesh->mTangents[i].x;
            vector.y = mesh->mTangents[i].y;
            vector.z = mesh->mTangents[i].z;
            vertex.tangent = vector;
            native_vertex.tangent = vector;
            // bitangent
            vector.x = mesh->mBitangents[i].x;
            vector.y = mesh->mBitangents[i].y;
            vector.z = mesh->mBitangents[i].z;
            vertex.bitangent = vector;
            native_vertex.bitangent = vector;
        }
        else
            vertex.uv = glm::vec2(0.0f, 0.0f);

        vertices.push_back(vertex);
        model.data.vertices.push_back(vertex);
    }

    return vertices;
}

std::vector<res::bone> res::loader::model_loader::copy_bones(std::vector<res::Vertex>& vertices, aiMesh* mesh, std::size_t base_vertex)
{
    std::vector<res::bone> bones;
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

            for (int iw = 0; iw < vertex.bones_weight.length(); ++iw)
            {
                if (vertex.bones_weight[iw] == 0.0) {
                    vertex.bones[iw] = bone_id;
                    vertex.bones_weight[iw] = weight;
                    break;
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

res::Tag res::loader::model_loader::find_material_texture(aiMaterial* mat, aiTextureType type) {
    for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
    {
        aiString str;
        mat->GetTexture(type, i, &str);
        std::string_view texture_name = str.C_Str();
        return tag + Tag::make(texture_name);
    }
    return {};
}

void res::loader::model_loader::batch_meshes()
{
    std::unordered_map<std::size_t, std::vector<std::size_t>> batches;
    bool is_need_batch = false;
    for (std::size_t idx = 0; idx < meshes.size(); ++idx)
    {
        auto& mesh = meshes[idx];
        std::size_t batch_idx = mesh.material.diffuse.get_hash()
            | mesh.material.ambient.get_hash()
            | mesh.material.normal.get_hash()
            | mesh.material.specular.get_hash();
        batches[batch_idx].push_back(idx);
    }

    std::vector<res::Mesh> batch_result;
    for (auto& [_, bmeshes] : batches)
    {
        res::Mesh new_mesh = meshes[bmeshes[0]];
        egLOG("load/batch", "batch {}", new_mesh.material.diffuse.get_full());

        for (std::size_t idx = 1; idx < bmeshes.size(); ++idx)
        {
            auto& mesh = meshes[bmeshes[idx]];

            std::size_t addition = new_mesh.vertices.size();

            new_mesh.vertices.reserve(new_mesh.vertices.size() + mesh.vertices.size());
            std::copy(mesh.vertices.begin(), mesh.vertices.end(), std::back_inserter(new_mesh.vertices));

            std::for_each(mesh.indices.begin(), mesh.indices.end(), [addition](unsigned int& inc) { inc += addition; });

            new_mesh.indices.reserve(new_mesh.indices.size() + mesh.indices.size());
            std::copy(mesh.indices.begin(), mesh.indices.end(), std::back_inserter(new_mesh.indices));
        }
        batch_result.push_back(new_mesh);
    }

    if (!batch_result.empty()) {
        egLOG("load/batching", "from {} meshes convert to {} meshes", meshes.size(), batch_result.size());

        meshes = batch_result;
    }
}