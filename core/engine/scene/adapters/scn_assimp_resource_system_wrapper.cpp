#include "scn_assimp_resource_system_wrapper.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_model_reference.h>
#include <assimp/MemoryIOWrapper.h>

Assimp::IOStream* scn::engine_assimp_resource_system_wrapper::Open(const char* pFile, const char* pMode)
{
    const std::optional<res::tag> reference = res::model_reference_tag(source_tag, pFile);
    if (!reference) {
        egLOG_WARN("loader", "model '{}' refers to '{}', which is absolute, in another protocol or above its root: not read",
            source_tag.view(), pFile);
        return nullptr;
    }
    const res::tag& file_tag = *reference;
    if (file_tag == source_tag) {
        return new Assimp::MemoryIOStream(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    if (std::string_view{ pMode } == "rb") {
        auto it = data_map.find(file_tag);
        if (it == data_map.end()) {
            auto fdata = resource_system.fetch_data(file_tag);
            if (!fdata) {
                return nullptr;
            }
            data_map[file_tag] = fdata;
            auto& data = fdata->get();
            return new Assimp::MemoryIOStream(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        } else {
            auto& data = it->second->get();
            return new Assimp::MemoryIOStream(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        }
    }

    return nullptr;
}
