#pragma once
#include <entt/fwd.hpp>
#include "ecs_system.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <cstddef>
#include <algorithm>
#include <iterator>

namespace scn
{
	struct animation_node; // defined in scn_mesh_nodes.hpp

	void init_animation_system(ecs::system_factory& factory);

	// Animation sampling helpers — exposed so unit tests exercise the real code
	// (single source of truth) instead of a copy. Used by scn_animation_job.cpp.

	// Returns the index i such that arr[i].time <= time < arr[i+1].time, clamped so that
	// i+1 is always valid. Returns 0 when fewer than 2 keys exist (template: T has .time).
	template<class T>
	std::size_t find_keyframe_index(const std::vector<T>& arr, float time)
	{
		if (arr.size() < 2) return 0;

		// Binary search: find the last key where key.time <= time
		auto it = std::upper_bound(arr.begin(), arr.end(), time,
			[](float t, const T& key) { return t < key.time; });

		if (it == arr.begin()) return 0;
		std::size_t idx = static_cast<std::size_t>(std::distance(arr.begin(), it)) - 1;
		// Clamp to second-to-last to ensure idx+1 is valid
		if (idx >= arr.size() - 1) idx = arr.size() - 2;
		return idx;
	}

	void calc_interpolated_scaling(glm::vec3& out, float time_ticks, const animation_node& anim);
	void calc_interpolated_position(glm::vec3& out, float time_ticks, const animation_node& anim);
	void calc_interpolated_rotation(glm::quat& out, float time_ticks, const animation_node& anim);
}