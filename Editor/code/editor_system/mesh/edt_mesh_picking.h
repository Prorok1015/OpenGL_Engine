#pragma once
#include "edt_mesh_adjacency.h"
#include "edt_mesh_model.h"
#include "edt_mesh_selection.h"
#include <snk/common/ds/ds_bvh.hpp>
#include <snk/common/ds/ds_intersect.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include <glm/glm.hpp>

namespace edt::mesh
{
	// The preview image as the picking sees it: the camera's matrix and where the image lies on the screen, in pixels (y down).
	struct screen_view
	{
		glm::mat4 view_projection{ 1.f };
		glm::vec2 origin{ 0.f };
		glm::vec2 size{ 1.f };

		// nullopt for a point behind the camera.
		std::optional<glm::vec2> project(const glm::vec3& world) const;
		// From the near plane through the pixel; the direction has length 1. nullopt for a matrix that cannot be inverted.
		std::optional<ds::ray3> ray_through(const glm::vec2& pixel) const;
	};

	struct pixel_rect
	{
		glm::vec2 min{ 0.f };
		glm::vec2 max{ 0.f };

		bool contains(const glm::vec2& point) const { return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y; }
	};

	struct face_hit
	{
		face_id face = invalid_id;
		float distance = 0.f;
		bool back_face = false;
	};

	// The triangles of a mesh in a ds::bvh, with the face each one belongs to. A cache: built from the mesh, thrown away on a change
	// (is_current); a face the bake leaves out has no triangle and cannot be hit.
	class mesh_picker
	{
	public:
		explicit mesh_picker(const poly_mesh& mesh);

		bool is_current(const poly_mesh& mesh) const { return mesh.generation() == m_generation; }
		std::size_t triangle_count() const { return m_bvh.size(); }

		// The nearest face the ray meets within `t_max` (in units of the ray's direction).
		std::optional<face_hit> first_hit(const ds::ray3& ray, bool cull_back_faces = false, float t_max = std::numeric_limits<float>::infinity()) const;
		// Whether any triangle lies on the ray before `distance`.
		bool blocked(const ds::ray3& ray, float distance, bool cull_back_faces = false) const;

	private:
		struct triangle
		{
			glm::vec3 a{ 0.f };
			glm::vec3 b{ 0.f };
			glm::vec3 c{ 0.f };
			face_id face = invalid_id;
		};

		std::uint64_t m_generation = 0;
		ds::bvh<triangle> m_bvh;
	};

	// What a click or a box over the image is told about. `xray` ignores what stands in front.
	struct pick_query
	{
		const poly_mesh& mesh;
		const mesh_adjacency& adjacency;
		const mesh_picker& picker;
		const screen_view& view;
		bool xray = false;
	};

	// The face under the pixel: the first the ray meets.
	std::optional<face_id> pick_face(const pick_query& query, const glm::vec2& pixel);
	// The vertex nearest to the pixel within `radius` pixels among the ones nothing stands in front of.
	std::optional<vertex_id> pick_vertex(const pick_query& query, const glm::vec2& pixel, float radius);
	// The edge nearest to the pixel within `radius` pixels, among the ones nothing stands in front of at the nearest point.
	std::optional<edge_key> pick_edge(const pick_query& query, const glm::vec2& pixel, float radius);

	// A click: the element of the selection's mode under the pixel goes through `op`; nothing under it clears the selection for
	// `replace` and leaves it alone for the others. Returns whether something was under the pixel.
	bool click_select(mesh_selection& selection, const pick_query& query, const glm::vec2& pixel, float radius, select_op op);
	// A box: what lies in it (vertices, edges with both ends in it, faces with their centre in it) goes through `op`.
	void box_select(mesh_selection& selection, const pick_query& query, const pixel_rect& rect, select_op op);
}
