#pragma once
#include "res_tag.h"

namespace res
{
	struct animation
	{
		std::string name;
		float duration = 0.f;
		float ticks_per_second = 0.f;
	};

	template<class T>
	struct animation_key_t
	{
		T value;
		float time;
	};

	using animation_pos_key = animation_key_t<glm::vec3>;
	using animation_rotate_key = animation_key_t<glm::quat>;
	using animation_scale_key = animation_key_t<glm::vec3>;

	struct animation_node
	{
		std::vector<animation_pos_key> pos_keys;
		std::vector<animation_rotate_key> rotate_keys;
		std::vector<animation_scale_key> scale_keys;
	};

	struct mesh_view
	{
		std::size_t vx_begin = 0;
		std::size_t vx_end = 0;
		std::size_t ind_begin = 0;
		std::size_t ind_end = 0;

		std::size_t get_vertices_count() const { return vx_end - vx_begin; }
		std::size_t get_indices_count() const { return ind_end - ind_begin; }
	};
}