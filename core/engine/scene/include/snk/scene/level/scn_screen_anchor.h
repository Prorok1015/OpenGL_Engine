#pragma once
#include <snk/ecs/ecs_channel.hpp>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/common/snk_api.h>
#include <glm/glm.hpp>

namespace scn
{
	class context_manager;
	class context_set;

	// A marker over an object (WORLD_MESSAGING 3, markers): a UI element names its target by node path in
	// a context of the same set and, optionally, a world-space offset from the target's position. The
	// render extractor resolves it every frame after all updates, so the position is never a frame late.
	// An empty `target.context` is the context of the element itself; a context of another set cannot be
	// named (the lookup stays inside the set of the element).
	struct screen_anchor_component
	{
		ecs::entity_ref target;
		glm::vec3 offset{ 0.f };
	};

	// What the extractor wrote for the element this frame. Without a resolved target (no such context,
	// node, transform or camera with a viewport) `resolved` is false and `off_screen` is true.
	struct screen_anchor_result
	{
		// Pixels from the top-left corner of the viewport of the camera that sees the target, y down.
		glm::vec2 position{ 0.f };
		// The same position as a fraction of that viewport (0..1 on the screen).
		glm::vec2 normalized{ 0.f };
		bool resolved = false;
		// Behind the camera (or its near plane), beyond the far plane or outside the viewport.
		bool off_screen = true;
		// Behind the camera: `position` has no meaning (it is zero).
		bool behind_camera = false;
	};

	// The projection matrix of a camera for a viewport (perspective, or orthographic by `ortho_size` world
	// units of height). The render extractor and the anchors use the same one.
	SNK_API glm::mat4 camera_projection_matrix(const camera_component& cam, const glm::ivec4& viewport);

	// Projects a world position through the camera onto the viewport (x, y, width, height; width and
	// height at least 1). Pure math, no registry.
	SNK_API screen_anchor_result project_to_screen(const glm::mat4& view, const glm::mat4& projection,
												   const glm::ivec4& viewport, const glm::vec3& world_position);

	// Fills a screen_anchor_result on every element with a screen_anchor_component in the contexts of
	// the set that are extracted. The camera is the one of the target's context that has a viewport
	// (`backbuffer_size` stands in for the viewport of backbuffer cameras): the lowest layer first,
	// then the first in storage order. Runs outside the phases, after all updates of the frame.
	SNK_API void resolve_screen_anchors(context_set& set, const glm::ivec2& backbuffer_size);
	SNK_API void resolve_screen_anchors(context_manager& contexts, const glm::ivec2& backbuffer_size);
}
