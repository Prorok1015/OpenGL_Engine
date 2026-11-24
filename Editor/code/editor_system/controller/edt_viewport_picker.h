#pragma once
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <vector>

namespace edt
{
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
	// (scn::raycast_all), hidden meshes skipped. A hit on an entity that is no node (it has no node_id_component) belongs to its
	// nearest ancestor that is one; a hit with no such ancestor is an editor's own entity and the ray goes on behind it.
	// Nothing for an empty image, a matrix that cannot be inverted, or a ray that meets no node. Skinned meshes are hit in their
	// bind pose and a geometry that is not loaded is not hit (the limits of scn::raycast_closest).
	[[nodiscard]] std::optional<picked_node> pick_node(entt::registry& world, const pick_point& point);
}
