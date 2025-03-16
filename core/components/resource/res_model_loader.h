#pragma once
#include <common.h>
#include <assimp/scene.h>
#include <stack>

#include "res_mesh.hpp"
#include "res_tag.h"

namespace res::loader {
    class model_loader
    {
    public:
        model_loader(const tag& tag_)
            : tag(tag_)
        {
        }

        std::vector<Mesh> load();

        void process_node(aiNode* node, const aiScene* scene, res::node_hierarchy_view& hierarchy);

        Mesh copy_mesh(aiMesh* mesh, const aiScene* scene, res::node_hierarchy_view& hierarchy);

        Material copy_material(const aiScene* scene, aiMaterial* mat);

        std::vector<res::Vertex> copy_vertices(aiMesh* mesh);

        std::vector<res::bone> copy_bones(std::vector<res::Vertex>& vertices, aiMesh* mesh, std::size_t base_vertex);

        std::vector<unsigned int> copy_indeces(aiMesh* mesh);

        tag find_material_texture(const aiScene* scene, aiMaterial* mat, aiTextureType type);

        void increase_txt_counter(const res::tag& txt)
        {
            if (txt.is_valid()) {
                textures_counter[txt]++;
            }
        }

        void enclose_hierarchy(res::node_hierarchy_view& node, const aiScene* scene);

        void batch_meshes();
        void trace_node(const std::stack<aiNode*>& stack);

        model_presintation model;

    private:
        tag tag;

        std::size_t max_bones_count = 0;

        std::vector<std::string> dbgAnimatedNodes;

        std::unordered_map<std::string, std::size_t> bones_mapping;
        std::unordered_map<std::size_t, std::vector<int32_t>> vertex_bone_mapping;

        std::vector<Mesh> meshes;
        std::stack<glm::mat4> transform_stack;
        std::stack<aiNode*> nodes;
        std::unordered_map<res::tag, int> textures_counter;
        std::unordered_map<aiMesh*, int> meshes_counter;
        std::size_t deep = 0;
    };
}
