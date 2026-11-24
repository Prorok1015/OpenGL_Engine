#include "edt_mesh_picking.h"
#include "edt_mesh_bake.h"
#include <snk/common/engine_assert.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace edt::mesh
{
	namespace
	{
		// What is in front of the camera by less than this counts as behind it (project() gives nothing for it).
		constexpr float k_min_depth = 1e-4f;
		// A segment is cut here, well inside what project() accepts: a cut at k_min_depth itself could land on either side of it
		// by rounding, and a point close to the camera plane lies far off the image.
		constexpr float k_clip_depth = 1e-2f;

		void check_current(const pick_query& query)
		{
			ASSERT_MSG(query.picker.is_current(query.mesh) && query.adjacency.is_current(query.mesh), "the picker or the adjacency is older than the mesh");
		}

		// A hit this close in front of a point is the point's own surface: a part of the mesh's size, and what float math loses far away.
		float tolerance_at(const mesh_picker& picker, float distance)
		{
			return 1e-3f * picker.extent() + 4e-7f * distance;
		}

		bool visible_at(const pick_query& query, const glm::vec3& point, const glm::vec2& pixel)
		{
			if (query.xray) return true;
			const std::optional<ds::ray3> ray = query.view.ray_through(pixel);
			if (!ray) return false;
			const float distance = glm::dot(point - ray->origin, ray->direction);
			return !query.picker.blocked(*ray, distance - tolerance_at(query.picker, distance));
		}

		struct segment_pick
		{
			float distance_squared = 0.f;
			glm::vec2 nearest{ 0.f };
		};

		segment_pick nearest_on_segment(const glm::vec2& point, const glm::vec2& a, const glm::vec2& b)
		{
			const glm::vec2 along = b - a;
			const float length_squared = glm::dot(along, along);
			const float s = length_squared > 0.f ? std::clamp(glm::dot(point - a, along) / length_squared, 0.f, 1.f) : 0.f;
			const glm::vec2 nearest = a + along * s;
			return { glm::dot(point - nearest, point - nearest), nearest };
		}

		// The point of the segment nearest to the (infinite) line of the ray.
		glm::vec3 nearest_on_edge_to_ray(const ds::ray3& ray, const glm::vec3& from, const glm::vec3& to)
		{
			const glm::vec3 along = to - from;
			const glm::vec3 offset = ray.origin - from;
			const float b = glm::dot(ray.direction, along);
			const float c = glm::dot(along, along);
			const float d = glm::dot(ray.direction, offset);
			const float e = glm::dot(along, offset);
			const float denominator = c - b * b;
			const float s = denominator > 1e-12f ? std::clamp((e - b * d) / denominator, 0.f, 1.f) : 0.f;
			return from + along * s;
		}

		std::optional<glm::vec3> position_of(const poly_mesh& mesh, vertex_id id)
		{
			const vertex* v = mesh.find_vertex(id);
			if (!v) return std::nullopt;
			return v->position;
		}

		std::vector<vertex_id> used_vertices(const pick_query& query)
		{
			std::vector<vertex_id> ids;
			for (const auto& [id, v] : query.mesh.vertices()) {
				if (!query.adjacency.faces_of_vertex(id).empty()) ids.push_back(id);
			}
			return ids;
		}
	}

	screen_view::screen_view(const glm::mat4& view_projection, const glm::vec2& origin, const glm::vec2& size)
		: m_view_projection(view_projection), m_inverse(glm::inverse(view_projection)), m_origin(origin), m_size(size)
	{
	}

	float screen_view::depth(const glm::vec3& world) const
	{
		return (m_view_projection * glm::vec4(world, 1.f)).w;
	}

	std::optional<glm::vec2> screen_view::project(const glm::vec3& world) const
	{
		const glm::vec4 clip = m_view_projection * glm::vec4(world, 1.f);
		if (!(clip.w > k_min_depth)) return std::nullopt;
		const glm::vec2 ndc{ clip.x / clip.w, clip.y / clip.w };
		return glm::vec2{ m_origin.x + (ndc.x * 0.5f + 0.5f) * m_size.x, m_origin.y + (0.5f - ndc.y * 0.5f) * m_size.y };
	}

	std::optional<ds::ray3> screen_view::ray_through(const glm::vec2& pixel) const
	{
		if (!(m_size.x > 0.f) || !(m_size.y > 0.f)) return std::nullopt;
		const float x = (pixel.x - m_origin.x) / m_size.x * 2.f - 1.f;
		const float y = 1.f - (pixel.y - m_origin.y) / m_size.y * 2.f;
		const glm::vec4 near_point = m_inverse * glm::vec4(x, y, -1.f, 1.f);
		const glm::vec4 far_point = m_inverse * glm::vec4(x, y, 1.f, 1.f);
		if (!std::isfinite(near_point.w) || !std::isfinite(far_point.w) || near_point.w == 0.f || far_point.w == 0.f) return std::nullopt;
		const glm::vec3 from = glm::vec3(near_point) / near_point.w;
		const glm::vec3 to = glm::vec3(far_point) / far_point.w;
		const glm::vec3 direction = to - from;
		const float length = glm::length(direction);
		if (!std::isfinite(length) || length <= 0.f) return std::nullopt;
		return ds::ray3(from, direction / length);
	}

	std::optional<std::pair<glm::vec3, glm::vec3>> screen_view::clip_segment(const glm::vec3& from, const glm::vec3& to) const
	{
		const float depth_from = depth(from);
		const float depth_to = depth(to);
		const bool from_in = depth_from >= k_clip_depth;
		const bool to_in = depth_to >= k_clip_depth;
		if (!from_in && !to_in) return std::nullopt;
		if (from_in && to_in) return std::pair{ from, to };
		const float s = (k_clip_depth - depth_from) / (depth_to - depth_from);
		const glm::vec3 cut = from + (to - from) * s;
		return from_in ? std::pair{ from, cut } : std::pair{ cut, to };
	}

	mesh_picker::mesh_picker(const poly_mesh& mesh)
		: m_generation(mesh.generation())
	{
		std::vector<ds::bvh<triangle>::item> items;
		items.reserve(mesh.triangle_count());
		for (const auto& [id, f] : mesh.faces()) {
			const std::optional<std::vector<std::array<std::uint32_t, 3>>> cut = triangulate_face(mesh, id);
			if (!cut) continue;
			float largest = -1.f;
			for (const std::array<std::uint32_t, 3>& corners : *cut) {
				triangle tri;
				tri.face = id;
				tri.a = mesh.find_vertex(f.corners[corners[0]].vertex)->position;
				tri.b = mesh.find_vertex(f.corners[corners[1]].vertex)->position;
				tri.c = mesh.find_vertex(f.corners[corners[2]].vertex)->position;
				const float area = glm::length(glm::cross(tri.b - tri.a, tri.c - tri.a));
				if (area > largest) {
					largest = area;
					m_face_points[id] = (tri.a + tri.b + tri.c) / 3.f;
				}
				ds::aabb3 box;
				ds::expand(box, tri.a);
				ds::expand(box, tri.b);
				ds::expand(box, tri.c);
				items.push_back({ box, tri });
			}
		}
		m_bvh.build(std::move(items));
	}

	std::optional<face_hit> mesh_picker::first_hit(const ds::ray3& ray, bool cull_back_faces, float t_max) const
	{
		std::optional<face_hit> best;
		float nearest = t_max;
		m_bvh.traverse_ray(ray, t_max, [&](std::uint32_t, const triangle& tri, float) {
			const auto hit = ds::ray_triangle(ray.origin, ray.direction, tri.a, tri.b, tri.c, cull_back_faces, nearest);
			if (hit && (!best || hit->t < nearest || (hit->t == nearest && tri.face < best->face))) {
				nearest = hit->t;
				best = face_hit{ tri.face, hit->t, hit->back_face };
			}
			return ds::bvh_ray_action{ nearest, false };
		});
		return best;
	}

	bool mesh_picker::blocked(const ds::ray3& ray, float distance, bool cull_back_faces) const
	{
		bool found = false;
		m_bvh.traverse_ray(ray, distance, [&](std::uint32_t, const triangle& tri, float) {
			if (ds::ray_triangle(ray.origin, ray.direction, tri.a, tri.b, tri.c, cull_back_faces, distance)) {
				found = true;
				return ds::bvh_ray_action{ distance, true };
			}
			return ds::bvh_ray_action{ distance, false };
		});
		return found;
	}

	std::optional<glm::vec3> mesh_picker::face_point(face_id id) const
	{
		const auto found = m_face_points.find(id);
		if (found == m_face_points.end()) return std::nullopt;
		return found->second;
	}

	std::optional<face_id> pick_face(const pick_query& query, const glm::vec2& pixel)
	{
		check_current(query);
		const std::optional<ds::ray3> ray = query.view.ray_through(pixel);
		if (!ray) return std::nullopt;
		const std::optional<face_hit> hit = query.picker.first_hit(*ray);
		if (!hit) return std::nullopt;
		return hit->face;
	}

	std::optional<vertex_id> pick_vertex(const pick_query& query, const glm::vec2& pixel, float radius)
	{
		check_current(query);
		struct candidate
		{
			float distance_squared = 0.f;
			vertex_id id = invalid_id;
			glm::vec3 position{ 0.f };
			glm::vec2 screen{ 0.f };
		};
		const auto farther = [](const candidate& lhs, const candidate& rhs) {
			return lhs.distance_squared != rhs.distance_squared ? lhs.distance_squared > rhs.distance_squared : lhs.id > rhs.id;
		};
		std::vector<candidate> candidates;
		for (vertex_id id : used_vertices(query)) {
			const std::optional<glm::vec3> position = position_of(query.mesh, id);
			const std::optional<glm::vec2> screen = position ? query.view.project(*position) : std::nullopt;
			if (!screen) continue;
			const float distance_squared = glm::dot(*screen - pixel, *screen - pixel);
			if (distance_squared <= radius * radius) candidates.push_back({ distance_squared, id, *position, *screen });
		}
		std::ranges::make_heap(candidates, farther);
		while (!candidates.empty()) {
			std::ranges::pop_heap(candidates, farther);
			const candidate nearest = candidates.back();
			candidates.pop_back();
			if (visible_at(query, nearest.position, nearest.screen)) return nearest.id;
		}
		return std::nullopt;
	}

	std::optional<edge_key> pick_edge(const pick_query& query, const glm::vec2& pixel, float radius)
	{
		check_current(query);
		struct candidate
		{
			float distance_squared = 0.f;
			edge_key key;
			glm::vec2 nearest{ 0.f };
			glm::vec3 from{ 0.f };
			glm::vec3 to{ 0.f };
		};
		const auto farther = [](const candidate& lhs, const candidate& rhs) {
			return lhs.distance_squared != rhs.distance_squared ? lhs.distance_squared > rhs.distance_squared : lhs.key > rhs.key;
		};
		std::vector<candidate> candidates;
		for (const edge_key& key : query.adjacency.edges()) {
			const std::optional<glm::vec3> from = position_of(query.mesh, key.a);
			const std::optional<glm::vec3> to = position_of(query.mesh, key.b);
			if (!from || !to) continue;
			const auto clipped = query.view.clip_segment(*from, *to);
			if (!clipped) continue;
			const std::optional<glm::vec2> screen_from = query.view.project(clipped->first);
			const std::optional<glm::vec2> screen_to = query.view.project(clipped->second);
			if (!screen_from || !screen_to) continue;
			const segment_pick nearest = nearest_on_segment(pixel, *screen_from, *screen_to);
			if (nearest.distance_squared <= radius * radius) {
				candidates.push_back({ nearest.distance_squared, key, nearest.nearest, clipped->first, clipped->second });
			}
		}
		std::ranges::make_heap(candidates, farther);
		while (!candidates.empty()) {
			std::ranges::pop_heap(candidates, farther);
			const candidate nearest = candidates.back();
			candidates.pop_back();
			if (query.xray) return nearest.key;
			const std::optional<ds::ray3> ray = query.view.ray_through(nearest.nearest);
			if (!ray) continue;
			const glm::vec3 point = nearest_on_edge_to_ray(*ray, nearest.from, nearest.to);
			const float distance = glm::dot(point - ray->origin, ray->direction);
			if (!query.picker.blocked(*ray, distance - tolerance_at(query.picker, distance))) return nearest.key;
		}
		return std::nullopt;
	}

	bool click_select(mesh_selection& selection, const pick_query& query, const glm::vec2& pixel, float radius, select_op op)
	{
		bool found = false;
		switch (selection.mode()) {
		case select_mode::vertex:
			if (const auto id = pick_vertex(query, pixel, radius)) {
				selection.apply_vertex(*id, op);
				found = true;
			}
			break;
		case select_mode::edge:
			if (const auto key = pick_edge(query, pixel, radius)) {
				selection.apply_edge(*key, op);
				found = true;
			}
			break;
		case select_mode::face:
			if (const auto id = pick_face(query, pixel)) {
				selection.apply_face(*id, op);
				found = true;
			}
			break;
		}
		if (!found && op == select_op::replace) selection.clear();
		return found;
	}

	void box_select(mesh_selection& selection, const pick_query& query, const pixel_rect& rect, select_op op)
	{
		check_current(query);
		const glm::vec2 low = glm::min(rect.min, rect.max);
		const glm::vec2 high = glm::max(rect.min, rect.max);
		const auto seen_point = [&](const glm::vec3& point) {
			const std::optional<glm::vec2> screen = query.view.project(point);
			if (!screen || screen->x < low.x || screen->x > high.x || screen->y < low.y || screen->y > high.y) return false;
			return visible_at(query, point, *screen);
		};
		std::unordered_map<vertex_id, bool> vertex_seen;
		const auto seen_vertex = [&](vertex_id id) {
			const auto known = vertex_seen.find(id);
			if (known != vertex_seen.end()) return known->second;
			const std::optional<glm::vec3> position = position_of(query.mesh, id);
			return vertex_seen.emplace(id, position && seen_point(*position)).first->second;
		};

		std::vector<vertex_id> vertices;
		std::vector<edge_key> edges;
		std::vector<face_id> faces;
		switch (selection.mode()) {
		case select_mode::vertex:
			for (vertex_id id : used_vertices(query)) {
				if (seen_vertex(id)) vertices.push_back(id);
			}
			break;
		case select_mode::edge:
			for (const edge_key& key : query.adjacency.edges()) {
				if (seen_vertex(key.a) && seen_vertex(key.b)) edges.push_back(key);
			}
			break;
		case select_mode::face:
			for (const auto& [id, f] : query.mesh.faces()) {
				const std::optional<glm::vec3> point = query.picker.face_point(id);
				if (point && seen_point(*point)) faces.push_back(id);
			}
			break;
		}

		if (op == select_op::replace) selection.clear();
		const select_op each = op == select_op::toggle ? select_op::toggle : select_op::add;
		for (vertex_id id : vertices) selection.apply_vertex(id, each);
		for (const edge_key& key : edges) selection.apply_edge(key, each);
		for (face_id id : faces) selection.apply_face(id, each);
	}
}
