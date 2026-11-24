#include "edt_mesh_picking.h"
#include "edt_mesh_bake.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace edt::mesh
{
	namespace
	{
		// A hit this close in front of a point is the point's own surface.
		float tolerance_at(float distance)
		{
			return 1e-3f * std::max(distance, 1.f);
		}

		bool visible_at(const pick_query& query, const glm::vec3& point, const glm::vec2& pixel)
		{
			if (query.xray) return true;
			const std::optional<ds::ray3> ray = query.view.ray_through(pixel);
			if (!ray) return false;
			const float distance = glm::dot(point - ray->origin, ray->direction);
			return !query.picker.blocked(*ray, distance - tolerance_at(distance));
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

	std::optional<glm::vec2> screen_view::project(const glm::vec3& world) const
	{
		const glm::vec4 clip = view_projection * glm::vec4(world, 1.f);
		if (!(clip.w > 1e-6f)) return std::nullopt;
		const glm::vec2 ndc{ clip.x / clip.w, clip.y / clip.w };
		return glm::vec2{ origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - ndc.y * 0.5f) * size.y };
	}

	std::optional<ds::ray3> screen_view::ray_through(const glm::vec2& pixel) const
	{
		if (!(size.x > 0.f) || !(size.y > 0.f)) return std::nullopt;
		const float x = (pixel.x - origin.x) / size.x * 2.f - 1.f;
		const float y = 1.f - (pixel.y - origin.y) / size.y * 2.f;
		const glm::mat4 inverse = glm::inverse(view_projection);
		glm::vec4 near_point = inverse * glm::vec4(x, y, -1.f, 1.f);
		glm::vec4 far_point = inverse * glm::vec4(x, y, 1.f, 1.f);
		if (!std::isfinite(near_point.w) || !std::isfinite(far_point.w) || near_point.w == 0.f || far_point.w == 0.f) return std::nullopt;
		const glm::vec3 from = glm::vec3(near_point) / near_point.w;
		const glm::vec3 to = glm::vec3(far_point) / far_point.w;
		const glm::vec3 direction = to - from;
		const float length = glm::length(direction);
		if (!std::isfinite(length) || length <= 0.f) return std::nullopt;
		return ds::ray3(from, direction / length);
	}

	mesh_picker::mesh_picker(const poly_mesh& mesh)
		: m_generation(mesh.generation())
	{
		std::vector<ds::bvh<triangle>::item> items;
		items.reserve(mesh.triangle_count());
		for (const auto& [id, f] : mesh.faces()) {
			const std::optional<std::vector<std::array<std::uint32_t, 3>>> cut = triangulate_face(mesh, id);
			if (!cut) continue;
			for (const std::array<std::uint32_t, 3>& corners : *cut) {
				triangle tri;
				tri.face = id;
				tri.a = mesh.find_vertex(f.corners[corners[0]].vertex)->position;
				tri.b = mesh.find_vertex(f.corners[corners[1]].vertex)->position;
				tri.c = mesh.find_vertex(f.corners[corners[2]].vertex)->position;
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
			if (hit && hit->t <= nearest) {
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

	std::optional<face_id> pick_face(const pick_query& query, const glm::vec2& pixel)
	{
		const std::optional<ds::ray3> ray = query.view.ray_through(pixel);
		if (!ray) return std::nullopt;
		const std::optional<face_hit> hit = query.picker.first_hit(*ray);
		if (!hit) return std::nullopt;
		return hit->face;
	}

	std::optional<vertex_id> pick_vertex(const pick_query& query, const glm::vec2& pixel, float radius)
	{
		std::vector<std::pair<float, vertex_id>> candidates;
		for (vertex_id id : used_vertices(query)) {
			const std::optional<glm::vec3> position = position_of(query.mesh, id);
			const std::optional<glm::vec2> screen = position ? query.view.project(*position) : std::nullopt;
			if (!screen) continue;
			const float distance_squared = glm::dot(*screen - pixel, *screen - pixel);
			if (distance_squared <= radius * radius) candidates.emplace_back(distance_squared, id);
		}
		std::ranges::sort(candidates);
		for (const auto& [distance_squared, id] : candidates) {
			const glm::vec3 position = *position_of(query.mesh, id);
			if (visible_at(query, position, *query.view.project(position))) return id;
		}
		return std::nullopt;
	}

	std::optional<edge_key> pick_edge(const pick_query& query, const glm::vec2& pixel, float radius)
	{
		struct candidate
		{
			float distance_squared = 0.f;
			edge_key key;
			glm::vec2 nearest{ 0.f };
		};
		std::vector<candidate> candidates;
		for (const edge_key& key : query.adjacency.edges()) {
			const std::optional<glm::vec3> from = position_of(query.mesh, key.a);
			const std::optional<glm::vec3> to = position_of(query.mesh, key.b);
			if (!from || !to) continue;
			const std::optional<glm::vec2> screen_from = query.view.project(*from);
			const std::optional<glm::vec2> screen_to = query.view.project(*to);
			if (!screen_from || !screen_to) continue;
			const segment_pick nearest = nearest_on_segment(pixel, *screen_from, *screen_to);
			if (nearest.distance_squared <= radius * radius) candidates.push_back({ nearest.distance_squared, key, nearest.nearest });
		}
		std::ranges::sort(candidates, [](const candidate& lhs, const candidate& rhs) {
			return lhs.distance_squared != rhs.distance_squared ? lhs.distance_squared < rhs.distance_squared : lhs.key < rhs.key;
		});
		for (const candidate& each : candidates) {
			if (query.xray) return each.key;
			const std::optional<ds::ray3> ray = query.view.ray_through(each.nearest);
			if (!ray) continue;
			const glm::vec3 point = nearest_on_edge_to_ray(*ray, *position_of(query.mesh, each.key.a), *position_of(query.mesh, each.key.b));
			const float distance = glm::dot(point - ray->origin, ray->direction);
			if (!query.picker.blocked(*ray, distance - tolerance_at(distance))) return each.key;
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
		const auto inside = [&](const glm::vec3& point) -> std::optional<glm::vec2> {
			const std::optional<glm::vec2> screen = query.view.project(point);
			if (!screen || !rect.contains(*screen)) return std::nullopt;
			return screen;
		};
		const auto seen = [&](const glm::vec3& point) {
			const std::optional<glm::vec2> screen = inside(point);
			return screen && visible_at(query, point, *screen);
		};

		std::vector<vertex_id> vertices;
		std::vector<edge_key> edges;
		std::vector<face_id> faces;
		switch (selection.mode()) {
		case select_mode::vertex:
			for (vertex_id id : used_vertices(query)) {
				if (seen(*position_of(query.mesh, id))) vertices.push_back(id);
			}
			break;
		case select_mode::edge:
			for (const edge_key& key : query.adjacency.edges()) {
				if (seen(*position_of(query.mesh, key.a)) && seen(*position_of(query.mesh, key.b))) edges.push_back(key);
			}
			break;
		case select_mode::face:
			for (const auto& [id, f] : query.mesh.faces()) {
				if (f.corners.empty()) continue;
				glm::vec3 centre{ 0.f };
				for (const corner& c : f.corners) centre += query.mesh.find_vertex(c.vertex)->position;
				centre /= static_cast<float>(f.corners.size());
				if (seen(centre)) faces.push_back(id);
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
