#pragma once
#include "edt_mesh_adjacency.h"
#include "edt_mesh_model.h"
#include "edt_mesh_selection.h"
#include <snk/common/ds/ds_bvh.hpp>
#include <snk/common/ds/ds_intersect.hpp>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace edt::mesh
{
	// The preview image as the picking sees it: the camera's matrix and where the image lies on the screen, in pixels (y down).
	// The inverse of the matrix is taken once, here.
	class screen_view
	{
	public:
		screen_view() = default;
		screen_view(const glm::mat4& view_projection, const glm::vec2& origin, const glm::vec2& size);

		const glm::mat4& view_projection() const { return m_view_projection; }
		const glm::vec2& origin() const { return m_origin; }
		const glm::vec2& size() const { return m_size; }

		// What a point is in front of the camera by (the w of its clip position): not above zero for a point behind it.
		float depth(const glm::vec3& world) const;
		// nullopt for a point behind the camera.
		std::optional<glm::vec2> project(const glm::vec3& world) const;
		// From the near plane through the pixel; the direction has length 1. nullopt for a matrix that cannot be inverted.
		std::optional<ds::ray3> ray_through(const glm::vec2& pixel) const;
		// The part of a segment that is in front of the camera; nullopt when none of it is.
		std::optional<std::pair<glm::vec3, glm::vec3>> clip_segment(const glm::vec3& from, const glm::vec3& to) const;

	private:
		glm::mat4 m_view_projection{ 1.f };
		glm::mat4 m_inverse{ 1.f };
		glm::vec2 m_origin{ 0.f };
		glm::vec2 m_size{ 1.f };
	};

	// Any two corners: box_select puts them in order.
	struct pixel_rect
	{
		glm::vec2 min{ 0.f };
		glm::vec2 max{ 0.f };
	};

	struct face_hit
	{
		face_id face = invalid_id;
		float distance = 0.f;
		bool back_face = false;
	};

	// The triangles of a mesh in a ds::bvh, with the face each one belongs to. A cache: built from the mesh, thrown away on a
	// change (is_current); a face the bake leaves out has no triangle, cannot be hit and has no face_point.
	class mesh_picker
	{
	public:
		explicit mesh_picker(const poly_mesh& mesh);

		bool is_current(const poly_mesh& mesh) const { return mesh.generation() == m_generation; }
		std::size_t triangle_count() const { return m_bvh.size(); }
		// The diagonal of the box of all triangles: what the occlusion tolerance is a part of.
		float extent() const { return m_bvh.empty() ? 0.f : glm::length(ds::size(m_bvh.bounds())); }

		// The nearest face the ray meets within `t_max` (in units of the ray's direction); of two at the same distance the
		// lower face id.
		std::optional<face_hit> first_hit(const ds::ray3& ray, bool cull_back_faces = false, float t_max = std::numeric_limits<float>::infinity()) const;
		// Whether any triangle lies on the ray before `distance`.
		bool blocked(const ds::ray3& ray, float distance, bool cull_back_faces = false) const;
		// A point that lies on the face itself, whatever its shape (concave and bent faces included): the centre of its
		// largest triangle. A face is "seen" in a box when this point is.
		std::optional<glm::vec3> face_point(face_id id) const;
		// The bake's cut of a face (corner indices of the face), kept from when the picker was made; null for a face the bake leaves out.
		const std::vector<std::array<std::uint32_t, 3>>* face_triangles(face_id id) const;

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
		std::map<face_id, glm::vec3> m_face_points;
		std::map<face_id, std::vector<std::array<std::uint32_t, 3>>> m_face_triangles;
	};

	// What a click or a box over the image is told about, all of the same generation of the mesh (asserted). `xray` ignores
	// what stands in front.
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
	// The edge nearest to the pixel within `radius` pixels, among the ones nothing stands in front of at the nearest point; the
	// part of an edge behind the camera does not count.
	std::optional<edge_key> pick_edge(const pick_query& query, const glm::vec2& pixel, float radius);

	// A click: the element of the selection's mode under the pixel goes through `op`; nothing under it clears the selection for
	// `replace` and leaves it alone for the others. Returns whether something was under the pixel.
	bool click_select(mesh_selection& selection, const pick_query& query, const glm::vec2& pixel, float radius, select_op op);
	// A box: what lies in it goes through `op`. Vertices by their position; edges when both ends are in it; faces when their
	// face_point is (a face the bake leaves out never). A point behind the camera is in no box.
	void box_select(mesh_selection& selection, const pick_query& query, const pixel_rect& rect, select_op op);
}
