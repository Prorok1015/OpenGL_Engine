#include "scn_skinning_manager.h"
#include "scn_skinning_prototype_desc.h"
#include "scn_model.h"

rnd::driver::ssbo_buffer_interface* scn::skinning_manager::get_weights_indeces_buffer(entt::handle ent,  rnd::driver::driver_interface* driver)
{
    const auto& tag = ent.get<scn::skinning_component>().skinning_tag;
    if (bone_indices_buffer.find(tag) == bone_indices_buffer.end()) {
        bone_indices_buffer[tag] = create_ssbo_weights_indeces_buffer(tag, driver);
    }
    return bone_indices_buffer[tag].get();
}

rnd::driver::ssbo_buffer_interface* scn::skinning_manager::get_bones_matrices_buffer(rnd::driver::driver_interface* driver)
{
    if (!bones_matrices_buffer) {
        bones_matrices_buffer = driver->create_ssbo_buffer(sizeof(glm::mat4) * 128, 0);
	}

	return bones_matrices_buffer.get();
}

rnd::driver::ssbo_buffer_interface* scn::skinning_manager::get_tmp_triangles_hightlight_buffer(rnd::driver::driver_interface* driver)
{
    if (!tmp_treangles_hightlight_buffer) {
        tmp_treangles_hightlight_buffer = driver->create_ssbo_buffer(sizeof(uint32_t) * 1024 * 1024, 0);
	}

	return tmp_treangles_hightlight_buffer.get();
}

std::unique_ptr<rnd::driver::ssbo_buffer_interface> scn::skinning_manager::create_ssbo_weights_indeces_buffer(res::tag skin, rnd::driver::driver_interface* driver)
{
     auto skinning = res::get_system().require_sync<scn::skinning_prototype_desc>(skin);
     auto columns = skinning->get_2d_array_bonesids_weights();

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