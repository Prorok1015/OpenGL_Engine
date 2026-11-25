#pragma once
#include "edt_mesh_model.h"
#include "edt_mesh_selection.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace edt::mesh
{
	// Where the transform's centre is: the average of the vertices (a vertex used by many faces counts once) or the middle of their box.
	enum class pivot_mode : std::uint8_t
	{
		median,
		bounds_centre,
	};

	// The axes the gizmo shows: the world's, or the average normal of the selection as Z.
	enum class gizmo_orientation : std::uint8_t
	{
		world,
		normal,
	};

	// A point and three orthonormal axes.
	struct pivot_frame
	{
		glm::vec3 origin = glm::vec3(0.f);
		glm::vec3 x = glm::vec3(1.f, 0.f, 0.f);
		glm::vec3 y = glm::vec3(0.f, 1.f, 0.f);
		glm::vec3 z = glm::vec3(0.f, 0.f, 1.f);
	};

	// The vertices the selection touches (a face's corners, an edge's ends) that the mesh has.
	std::set<vertex_id> selected_vertices(const poly_mesh& mesh, const mesh_selection& selection);

	// A number that changes whenever what is selected changes (same elements, same stamp). For a UI that must notice a new selection.
	std::uint64_t selection_stamp(const mesh_selection& selection);

	// nullopt for a selection that touches no vertex.
	std::optional<pivot_frame> selection_frame(const poly_mesh& mesh, const mesh_selection& selection, pivot_mode pivot, gizmo_orientation orientation);

	// The positions the vertices had when a gesture began: every transform of the gesture is made from these, never from the
	// positions of the last frame, so a long drag does not drift and going back to zero returns to the start.
	struct vertex_snapshot
	{
		std::vector<std::pair<vertex_id, glm::vec3>> positions;
	};

	// Vertices the mesh does not have are left out.
	vertex_snapshot capture_vertices(const poly_mesh& mesh, const std::set<vertex_id>& vertices);

	// Puts every vertex of the snapshot at `transform` applied to its snapshot position; a result that is not finite leaves the vertex
	// where it was. Returns how many vertices were set.
	std::size_t apply_transform(poly_mesh& mesh, const vertex_snapshot& snapshot, const glm::mat4& transform);

	glm::mat4 translation_matrix(const glm::vec3& delta);
	// Counter-clockwise seen along the axis towards the viewer; a zero axis gives the identity.
	glm::mat4 rotation_matrix(const glm::vec3& pivot, const glm::vec3& axis, float radians);
	// Scales along the axes of the frame about its origin; factors of 1 leave a direction as it is.
	glm::mat4 scale_matrix(const pivot_frame& frame, const glm::vec3& factors);

	// The nearest multiple of `step`; a step that is not above zero (or not finite) leaves the value alone.
	float snap_to_step(float value, float step);
	glm::vec3 snap_to_step(const glm::vec3& value, float step);
}
