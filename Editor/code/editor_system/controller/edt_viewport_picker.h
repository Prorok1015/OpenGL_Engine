#pragma once
#include <snk/scene/scn_spatial.h>
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <vector>

namespace edt
{
	// The layer (scn::spatial_layers) of an editor-only mesh entity: a light or camera icon, a helper shape. pick_node never hits
	// it, wherever it hangs in the hierarchy. Give such an entity `scn::spatial_layers{ scn::spatial_mask{ k_editor_helper_layer } }`.
	inline constexpr scn::spatial_layer k_editor_helper_layer{ 31 };

	// A point of the editor camera's image: the matrices the viewport draws with and the pixel (origin top left, y down) in the
	// image's own pixels.
	struct pick_point
	{
		glm::mat4 view{ 1.f };
		glm::mat4 projection{ 1.f };
		glm::vec2 pixel{ 0.f };
		glm::vec2 image_size{ 0.f };
	};

	struct picked_node
	{
		entt::entity entity;
		// The node the hit belongs to, as the hierarchy and selection_service name it: `node_id_component::path`.
		std::vector<std::uint64_t> path;
	};

	// The nearest node under the pixel: the ray through it (scn::ray_from_screen) against the world's triangles
	// (scn::raycast_all), hidden meshes and k_editor_helper_layer entities skipped. A hit on an entity that is no node (it has no
	// node_id_component) belongs to its nearest ancestor that is one (at most 256 levels up: a parent cycle is a miss, with a
	// warning); a hit with no such ancestor is not selectable and the walk goes on behind it. The ray starts on the camera's near
	// plane, so what lies between the eye and it is not hit.
	// Nothing for a pixel outside the image, a NaN or infinite pixel, image or matrix, a matrix that cannot be inverted, or a ray
	// that meets no node. Skinned meshes are hit in their bind pose and a geometry that is not loaded is not hit (the limits of
	// scn::raycast_closest).
	// Call it after the frame's transform pass (the world matrices and the spatial world's trees of this frame): from the panel's
	// on_render, never between a move and the transform system.
	[[nodiscard]] std::optional<picked_node> pick_node(entt::registry& world, const pick_point& point);
}
