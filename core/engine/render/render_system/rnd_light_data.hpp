#pragma once
#include <vector>
#include <glm/vec4.hpp>
#include "shader/rnd_shader_manager.h"

// rnd_light_data.hpp
// Transport-layer light data for frame_context.
// Lives in CPU memory, is populated by light extractors from ECS,
// and consumed by render passes to upload into GPU uniform buffers.

namespace rnd {

	// Per-frame aggregated light data extracted from ECS.
	// Stored in frame_context via:
	//   context.data.construct<rnd::scene_lights_t>(...)
	struct scene_lights_t {
		struct directional_light_t {
			glm::vec4 direction;
			glm::vec4 diffuse;
			glm::vec4 ambient;
			glm::vec4 specular;
		};

		std::vector<directional_light_t> directional;

		// Convert to GPU-ready lights_params for ShaderManager::update_global_sun().
		// Respects lights_params::MAX_LIGHT_COUNT limit.
		rnd::lights_params to_gpu_params() const {
			rnd::lights_params result{};
			const int count = static_cast<int>(
				std::min(directional.size(),
				         static_cast<std::size_t>(rnd::lights_params::MAX_LIGHT_COUNT)));
			for (int i = 0; i < count; ++i) {
				result.directional_lights[i].direction = directional[i].direction;
				result.directional_lights[i].diffuse   = directional[i].diffuse;
				result.directional_lights[i].ambient   = directional[i].ambient;
				result.directional_lights[i].specular  = directional[i].specular;
			}
			return result;
		}
	};

} // namespace rnd
