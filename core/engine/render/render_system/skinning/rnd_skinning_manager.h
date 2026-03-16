#pragma once
#include "common.h"
#include "res_tag.h"
#include "rnd_driver_interface.h"
#include "rnd_ssbo_buffer_interface.h"
#include <memory>
#include <unordered_map>
#include <glm/mat4x4.hpp>
#include <vector>

namespace rnd
{
	class skinning_manager
	{
	public:
		skinning_manager() = default;
		~skinning_manager() = default;
		skinning_manager(const skinning_manager&) = delete;
		skinning_manager& operator=(const skinning_manager&) = delete;
		skinning_manager(skinning_manager&&) = delete;
		skinning_manager& operator=(skinning_manager&&) = delete;

		void register_weights(res::tag tag, std::vector<std::vector<uint32_t>> weights) {
			m_weights_registry[tag] = std::move(weights);
		}

		rnd::driver::ssbo_buffer_interface* get_buffer(res::tag skin) const {
			if (auto it = bone_indices_buffer.find(skin); it != bone_indices_buffer.end()) {
				return it->second.get();
			}
			return nullptr;
		}

		void bind_skin(rnd::driver::driver_interface* drv, res::tag skin_tag, const std::vector<glm::mat4>& matrices);

		rnd::driver::ssbo_buffer_interface* get_weights_indeces_buffer(res::tag tag, rnd::driver::driver_interface* driver);
		rnd::driver::ssbo_buffer_interface* get_bones_matrices_buffer(rnd::driver::driver_interface* driver);
		rnd::driver::ssbo_buffer_interface* get_tmp_triangles_hightlight_buffer(rnd::driver::driver_interface* driver);
	private:
		std::unique_ptr<rnd::driver::ssbo_buffer_interface> create_ssbo_weights_indeces_buffer(res::tag skin, rnd::driver::driver_interface* driver);

	private:
		std::unordered_map<res::tag, std::vector<std::vector<uint32_t>>> m_weights_registry;
		std::unordered_map<res::tag, std::unique_ptr<rnd::driver::ssbo_buffer_interface>> bone_indices_buffer;
		std::unique_ptr<rnd::driver::ssbo_buffer_interface> bones_matrices_buffer;
		std::unique_ptr<rnd::driver::ssbo_buffer_interface> tmp_treangles_hightlight_buffer;
	};
}