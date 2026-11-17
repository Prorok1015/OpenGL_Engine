#pragma once
#include <cstdint>
#include <snk/common/common.h>
#include <snk/common/ds/ds_fixed_vector.hpp>
#include <snk/resource/res_tag.h>

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

	// What a camera clears before it draws (WORLD_MESSAGING section 7): the colour and the depth of its target,
	// only the depth (an overlay drawn over what an earlier camera left in the colour), or nothing.
	enum class camera_clear : uint8_t { all, depth, none };

	enum class camera_projection : uint8_t { perspective, ortho };

	// Height of the visible volume of an orthographic camera, in world units.
	inline constexpr float default_camera_ortho_size = 2.f;

	struct camera_component
	{
		float fov = 90.f;
		float near_distance = 0.0001f;
		float far_distance = 1000.f;
		viewport m_viewport;

		// Render layer: cameras draw in ascending layer order, each layer as a whole (all its passes) before the
		// next one starts, so a UI or HUD world on layer 1 lands on top of the 3D world on layer 0.
		int layer = 0;
		camera_clear clear = camera_clear::all;
		camera_projection projection = camera_projection::perspective;
		// Debug view, never saved: this camera draws the scene as wireframe (the editor viewport's toggle).
		bool wireframe = false;
		// Orthographic projection: the height of the visible volume in world units (the width follows the aspect).
		float ortho_size = default_camera_ortho_size;

		ds::fixed_vector<res::tag, 4> color_targets;  // [0] = main color RT
		res::tag depth_target;                         // depth RT
		res::tag tp_accum_target;                      // transparent accum RT
		res::tag tp_reveal_target;                     // transparent reveal RT
	};

}