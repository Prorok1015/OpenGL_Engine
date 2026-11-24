#include "edt_mesh_overlay.h"
#include "edt_mesh_bake.h"
#include <snk/common/engine_assert.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>

namespace edt::mesh
{
	namespace
	{
		std::optional<glm::vec3> position_of(const poly_mesh& mesh, vertex_id id)
		{
			const vertex* v = mesh.find_vertex(id);
			if (!v) return std::nullopt;
			return v->position;
		}

		// Newell's normal, not normalised; nullopt for a face with no area.
		std::optional<glm::vec3> face_normal(const poly_mesh& mesh, const face& f)
		{
			glm::vec3 sum{ 0.f };
			const std::size_t count = f.corners.size();
			for (std::size_t i = 0; i < count; ++i) {
				const std::optional<glm::vec3> current = position_of(mesh, f.corners[i].vertex);
				const std::optional<glm::vec3> next = position_of(mesh, f.corners[(i + 1) % count].vertex);
				if (!current || !next) return std::nullopt;
				sum.x += (current->y - next->y) * (current->z + next->z);
				sum.y += (current->z - next->z) * (current->x + next->x);
				sum.z += (current->x - next->x) * (current->y + next->y);
			}
			if (!(glm::dot(sum, sum) > 0.f) || !std::isfinite(sum.x + sum.y + sum.z)) return std::nullopt;
			return sum;
		}

		// Whether the face looks at the camera: judged by the ray through its centre (or, when the centre is out of sight, through a
		// corner that is in sight: a plane has one side whatever point of it is looked at), so a perspective and an orthographic camera
		// both work. A face with no area, and one none of whose points can be looked through (all behind the camera: the clipping
		// leaves nothing of it anyway), is not culled.
		bool faces_camera(const poly_mesh& mesh, const face& f, const screen_view& view)
		{
			const std::optional<glm::vec3> normal = face_normal(mesh, f);
			if (!normal) return true;
			glm::vec3 centre{ 0.f };
			for (const corner& c : f.corners) centre += mesh.find_vertex(c.vertex)->position;
			centre /= static_cast<float>(f.corners.size());
			const auto looks = [&](const glm::vec3& point) -> std::optional<bool> {
				const std::optional<glm::vec2> pixel = view.project(point);
				if (!pixel) return std::nullopt;
				const std::optional<ds::ray3> ray = view.ray_through(*pixel);
				if (!ray) return std::nullopt;
				return glm::dot(*normal, ray->direction) < 0.f;
			};
			if (const std::optional<bool> at_centre = looks(centre)) return *at_centre;
			for (const corner& c : f.corners) {
				if (const std::optional<bool> at_corner = looks(mesh.find_vertex(c.vertex)->position)) return *at_corner;
			}
			return true;
		}

		float distance_squared_to_segment(const glm::vec2& point, const glm::vec2& from, const glm::vec2& to)
		{
			const glm::vec2 along = to - from;
			const float length_squared = glm::dot(along, along);
			const float s = length_squared > 0.f ? std::clamp(glm::dot(point - from, along) / length_squared, 0.f, 1.f) : 0.f;
			const glm::vec2 nearest = from + along * s;
			return glm::dot(point - nearest, point - nearest);
		}

		void add_face_edges(const face& f, std::set<edge_key>& out)
		{
			const std::size_t count = f.corners.size();
			for (std::size_t i = 0; i < count; ++i) {
				out.insert(edge_key::of(f.corners[i].vertex, f.corners[(i + 1) % count].vertex));
			}
		}

		overlay_kind stronger(overlay_kind lhs, overlay_kind rhs)
		{
			return static_cast<int>(lhs) >= static_cast<int>(rhs) ? lhs : rhs;
		}

		void apply_budget(overlay_frame& frame, const overlay_options& options)
		{
			std::vector<overlay_segment>& segments = frame.segments;
			if (segments.size() > options.segment_budget) {
				const auto first_plain = std::stable_partition(segments.begin(), segments.end(),
					[](const overlay_segment& s) { return s.kind != overlay_kind::plain; });
				const std::size_t highlighted = static_cast<std::size_t>(first_plain - segments.begin());
				const std::size_t plain = segments.size() - highlighted;
				const std::size_t room = options.segment_budget > highlighted ? options.segment_budget - highlighted : 0;
				const std::size_t keep = std::min(room, plain);
				if (options.cursor && keep < plain) {
					const glm::vec2 cursor = *options.cursor;
					std::nth_element(first_plain, first_plain + static_cast<std::ptrdiff_t>(keep), segments.end(),
						[&cursor](const overlay_segment& lhs, const overlay_segment& rhs) {
							return distance_squared_to_segment(cursor, lhs.from, lhs.to) < distance_squared_to_segment(cursor, rhs.from, rhs.to);
						});
				}
				frame.omitted_segments = plain - keep;
				segments.erase(first_plain + static_cast<std::ptrdiff_t>(keep), segments.end());
			}
			std::ranges::stable_sort(segments, {}, [](const overlay_segment& s) { return static_cast<int>(s.kind); });
		}
	}

	overlay_frame build_overlay(const poly_mesh& mesh, const mesh_adjacency& adjacency, const mesh_selection& selection,
		const hover_target& hover, const screen_view& view, const overlay_options& options)
	{
		ASSERT_MSG(adjacency.is_current(mesh), "the adjacency is older than the mesh");
		overlay_frame frame;

		std::unordered_map<face_id, bool> facing;
		facing.reserve(mesh.faces().size());
		for (const auto& [id, f] : mesh.faces()) facing[id] = faces_camera(mesh, f, view);
		const auto shown = [&](std::span<const face_id> faces) {
			if (options.xray) return true;
			return std::ranges::any_of(faces, [&](face_id id) {
				const auto found = facing.find(id);
				return found != facing.end() && found->second;
			});
		};

		std::set<edge_key> selected_face_edges;
		for (face_id id : selection.faces()) {
			if (const face* f = mesh.find_face(id)) add_face_edges(*f, selected_face_edges);
		}
		std::set<edge_key> hovered_face_edges;
		if (selection.mode() == select_mode::face && hover.face) {
			if (const face* f = mesh.find_face(*hover.face)) add_face_edges(*f, hovered_face_edges);
		}

		const auto edge_kind = [&](const edge_key& key) {
			overlay_kind kind = overlay_kind::plain;
			if ((selection.mode() == select_mode::edge && hover.edge == key) || hovered_face_edges.contains(key)) kind = overlay_kind::hover;
			bool picked = false;
			switch (selection.mode()) {
			case select_mode::vertex:
				picked = selection.vertices().contains(key.a) && selection.vertices().contains(key.b);
				break;
			case select_mode::edge:
				picked = selection.edges().contains(key);
				break;
			case select_mode::face:
				picked = selected_face_edges.contains(key);
				break;
			}
			return picked ? stronger(kind, overlay_kind::selected) : kind;
		};

		for (const edge_key& key : adjacency.edges()) {
			if (!shown(adjacency.faces_of_edge(key))) continue;
			const std::optional<glm::vec3> from = position_of(mesh, key.a);
			const std::optional<glm::vec3> to = position_of(mesh, key.b);
			if (!from || !to) continue;
			const auto clipped = view.clip_segment(*from, *to);
			if (!clipped) continue;
			const std::optional<glm::vec2> screen_from = view.project(clipped->first);
			const std::optional<glm::vec2> screen_to = view.project(clipped->second);
			if (!screen_from || !screen_to) continue;
			frame.segments.push_back({ *screen_from, *screen_to, edge_kind(key) });
		}
		apply_budget(frame, options);

		if (selection.mode() == select_mode::vertex) {
			for (const auto& [id, v] : mesh.vertices()) {
				if (!shown(adjacency.faces_of_vertex(id))) continue;
				const std::optional<glm::vec2> screen = view.project(v.position);
				if (!screen) continue;
				overlay_kind kind = hover.vertex == id ? overlay_kind::hover : overlay_kind::plain;
				if (selection.vertices().contains(id)) kind = overlay_kind::selected;
				frame.points.push_back({ *screen, kind });
			}
			std::ranges::stable_sort(frame.points, {}, [](const overlay_point& p) { return static_cast<int>(p.kind); });
		}

		if (selection.mode() == select_mode::face) {
			const auto add_polygon = [&](face_id id, overlay_kind kind) {
				const face* f = mesh.find_face(id);
				if (!f || !shown(std::span<const face_id>(&id, 1))) return;
				std::optional<std::vector<std::array<std::uint32_t, 3>>> cut = triangulate_face(mesh, id);
				if (!cut) return;
				overlay_polygon polygon;
				polygon.kind = kind;
				polygon.triangles = std::move(*cut);
				polygon.corners.reserve(f->corners.size());
				for (const corner& c : f->corners) {
					const std::optional<glm::vec3> position = position_of(mesh, c.vertex);
					const std::optional<glm::vec2> screen = position ? view.project(*position) : std::nullopt;
					if (!screen) return;
					polygon.corners.push_back(*screen);
				}
				frame.polygons.push_back(std::move(polygon));
			};
			if (hover.face && !selection.faces().contains(*hover.face)) add_polygon(*hover.face, overlay_kind::hover);
			for (face_id id : selection.faces()) add_polygon(id, overlay_kind::selected);
		}
		return frame;
	}
}
