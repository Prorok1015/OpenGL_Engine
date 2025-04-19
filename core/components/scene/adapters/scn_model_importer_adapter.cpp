#include "scn_model_importer_adapter.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "res_system.h"

std::shared_ptr<res::Resource> scn::model_importer_adapter::operator()(const res::tag& tag, const std::vector<std::byte>& data) const
{
    // read file via ASSIMP
    Assimp::Importer importer;
	constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
    const aiScene* scene = importer.ReadFileFromMemory(data.data(), data.size(), flags);
    // check for errors
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) // if is Not Zero
    {
        egLOG("scene/model/load", "ERROR::ASSIMP::{}", importer.GetErrorString());
        return {};
    }

    auto& res_system = res::get_system();
    res_system.memory_resolver_.add_memory({}, {});

	return std::shared_ptr<res::Resource>();
}
