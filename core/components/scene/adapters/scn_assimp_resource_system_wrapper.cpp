#include "scn_assimp_resource_system_wrapper.h"
#include <assimp/MemoryIOWrapper.h>

Assimp::IOStream* scn::engine_assimp_resource_system_wrapper::Open(const char* pFile, const char* pMode)
{
    if (res::tag{ pFile } == source_tag) {
        return new Assimp::MemoryIOStream(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    if (pMode == "rb"sv) {
        res::tag file_tag = res::tag{ pFile };
        auto it = data_map.find(file_tag);
        if (it == data_map.end()) {
            auto fdata = resource_system.require_resource_data(file_tag);
            auto& data = data_map[file_tag] = fdata.get();
            return new Assimp::MemoryIOStream(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        }
        else {
            return new Assimp::MemoryIOStream(reinterpret_cast<const uint8_t*>(it->second.data()), it->second.size());
        }
    }

    return nullptr;
}
