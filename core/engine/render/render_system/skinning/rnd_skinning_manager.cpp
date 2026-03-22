#include "rnd_skinning_manager.h"
#include <vector>

rnd::driver::ssbo_buffer_interface* rnd::skinning_manager::get_weights_indeces_buffer(res::tag tag, rnd::driver::driver_interface* driver)
{
    if (bone_indices_buffer.find(tag) == bone_indices_buffer.end()) {
        if (auto buffer = create_ssbo_weights_indeces_buffer(tag, driver)) {
			bone_indices_buffer[tag] = std::move(buffer);
        } else {
			return nullptr;
        }
    }
    return bone_indices_buffer[tag].get();
}

rnd::driver::ssbo_buffer_interface* rnd::skinning_manager::get_bones_matrices_buffer(rnd::driver::driver_interface* driver)
{
    if (!bones_matrices_buffer) {
        bones_matrices_buffer = driver->create_ssbo_buffer(sizeof(glm::mat4) * 128, 0);
	}

	return bones_matrices_buffer.get();
}

rnd::driver::ssbo_buffer_interface* rnd::skinning_manager::get_tmp_triangles_hightlight_buffer(rnd::driver::driver_interface* driver)
{
    if (!tmp_treangles_hightlight_buffer) {
        tmp_treangles_hightlight_buffer = driver->create_ssbo_buffer(sizeof(uint32_t) * 1024 * 1024, 0);
	}

	return tmp_treangles_hightlight_buffer.get();
}

std::unique_ptr<rnd::driver::ssbo_buffer_interface> rnd::skinning_manager::create_ssbo_weights_indeces_buffer(res::tag skin, rnd::driver::driver_interface* driver)
{
     auto it = m_weights_registry.find(skin);
     if (it == m_weights_registry.end()) return nullptr;
     auto columns = it->second;

     uint32_t numColumns = static_cast<uint32_t>(columns.size());

     std::vector<uint32_t> offsets(numColumns + 1, 0);
     uint32_t currentOffset = 0;
     for (uint32_t j = 0; j < numColumns; ++j) {
         offsets[j] = currentOffset;
         currentOffset += static_cast<uint32_t>(columns[j].size());
     }
     offsets[numColumns] = currentOffset;

     std::vector<uint32_t> bufferData;
     bufferData.reserve(1 + (numColumns + 1) + currentOffset);

     bufferData.push_back(numColumns);

     bufferData.insert(bufferData.end(), offsets.begin(), offsets.end());

     for (const auto& col : columns) {
         bufferData.insert(bufferData.end(), col.begin(), col.end());
     }

     auto ssbo = driver->create_ssbo_buffer(bufferData.size() * sizeof(uint32_t), 0);
     ssbo->set_data(bufferData.data(), bufferData.size() * sizeof(uint32_t), 0);
     return ssbo;
}

void rnd::skinning_manager::bind_skin(rnd::driver::driver_interface* drv, res::tag skin_tag, std::span<const glm::mat4> matrices)
{
    if (!matrices.empty()) {
        if (auto* bones_buffer = get_bones_matrices_buffer(drv)) {
            bones_buffer->bind(2);
            bones_buffer->set_data(matrices.data(), matrices.size_bytes());
        }
    }

    if (skin_tag.is_valid()) {
        if (auto* ssbo = get_weights_indeces_buffer(skin_tag, drv)) {
            ssbo->bind(1);
        }
    }
}