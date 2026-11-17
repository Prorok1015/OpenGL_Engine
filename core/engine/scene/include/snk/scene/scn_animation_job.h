#pragma once
#include <entt/fwd.hpp>
#include <snk/ecs/ecs_system.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <cstddef>
#include <algorithm>
#include <iterator>
#include <snk/common/snk_api.h>
#include <snk/common/mem_allocator.h>
#include <memory_resource>
#include <unordered_map>

namespace scn
{
	struct animation_node; // defined in scn_mesh_nodes.hpp

	SNK_API void init_animation_system(ecs::system_factory& factory);

	// Animation sampling helpers - exposed so unit tests exercise the real code
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

	SNK_API void calc_interpolated_scaling(glm::vec3& out, float time_ticks, const animation_node& anim);
	SNK_API void calc_interpolated_position(glm::vec3& out, float time_ticks, const animation_node& anim);
	SNK_API void calc_interpolated_rotation(glm::quat& out, float time_ticks, const animation_node& anim);

	// The animated nodes of a skeleton all play the clip of its root, and the lookup walks every clip of the root
	// (animation_collection_component::find): asked once per root in a pass, not once per node (#742). The map is
	// frame-allocated, so an instance lives for one pass and nothing is kept across frames.
	template<class Clip>
	class clip_per_root
	{
	public:
		explicit clip_per_root(std::size_t expected_roots)
			: m_clips(ds::frame_allocator())
		{
			m_clips.reserve(expected_roots);
		}

		// What `lookup()` gives for `root`, called the first time the root is asked for; a root without a clip is
		// remembered as null.
		template<class Lookup>
		const Clip* get(entt::entity root, Lookup&& lookup)
		{
			const auto [it, inserted] = m_clips.try_emplace(root, nullptr);
			if (inserted) it->second = lookup();
			return it->second;
		}

	private:
		std::pmr::unordered_map<entt::entity, const Clip*> m_clips;
	};
}