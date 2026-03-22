#pragma once
#include "common.h"
#include "ds/ds_fixed_vector.hpp"

namespace scn
{
	struct viewport
	{
		glm::ivec2 lefttop{ 0 };
		glm::ivec2 size{ 0 };

		viewport() = default;
		viewport(const glm::ivec4& vp)
			: lefttop(vp.x, vp.y)
			, size(vp.z, vp.w) {}

		operator glm::ivec4() const noexcept {
			return { lefttop, size };
		}
	};

	struct camera_component
	{
		float fov = 90.f;
		float near_distance = 0.0001f;
		float far_distance = 1000.f;
		viewport m_viewport;

		ds::fixed_vector<res::tag, 4> color_targets;  // [0] = main color RT
		res::tag depth_target;                         // depth RT
		res::tag tp_accum_target;                      // transparent accum RT
		res::tag tp_reveal_target;                     // transparent reveal RT
	};

}