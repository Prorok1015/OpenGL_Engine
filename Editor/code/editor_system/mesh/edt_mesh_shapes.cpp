#include "edt_mesh_shapes.h"
#include <cmath>
#include <format>
#include <numbers>
#include <vector>

namespace edt::mesh
{
	namespace
	{
		constexpr float two_pi = 2.f * std::numbers::pi_v<float>;

		bool positive(float value)
		{
			return std::isfinite(value) && value > 0.f && value <= max_shape_extent;
		}

		bool in_range(std::uint32_t value, std::uint32_t min)
		{
			return value >= min && value <= max_shape_segments;
		}

		corner at(vertex_id vertex, float u, float v)
		{
			return corner{ vertex, glm::vec2{ u, v } };
		}

		struct ring
		{
			float radius = 0.f;
			float y = 0.f;
		};

		// Surface of revolution around Y: a pole on top, rings from top to bottom, a pole at the bottom.
		poly_mesh revolve(float top_y, const std::vector<ring>& rings, float bottom_y, std::uint32_t segments)
		{
			poly_mesh mesh;
			const vertex_id top = mesh.add_vertex({ 0.f, top_y, 0.f });
			std::vector<std::vector<vertex_id>> ring_vertices;
			ring_vertices.reserve(rings.size());
			for (const ring& r : rings) {
				std::vector<vertex_id>& ids = ring_vertices.emplace_back();
				ids.reserve(segments);
				for (std::uint32_t s = 0; s < segments; ++s) {
					const float phi = two_pi * static_cast<float>(s) / static_cast<float>(segments);
					ids.push_back(mesh.add_vertex({ r.radius * std::cos(phi), r.y, r.radius * std::sin(phi) }));
				}
			}
			const vertex_id bottom = mesh.add_vertex({ 0.f, bottom_y, 0.f });

			const float segment_count = static_cast<float>(segments);
			const float ring_span = static_cast<float>(rings.size() + 1);
			const auto u_at = [&](std::uint32_t s) { return static_cast<float>(s) / segment_count; };
			const auto v_at = [&](std::size_t i) { return static_cast<float>(i + 1) / ring_span; };

			for (std::uint32_t s = 0; s < segments; ++s) {
				const std::uint32_t next = (s + 1) % segments;
				mesh.add_face({ at(top, (static_cast<float>(s) + 0.5f) / segment_count, 0.f),
					at(ring_vertices.front()[next], u_at(s + 1), v_at(0)), at(ring_vertices.front()[s], u_at(s), v_at(0)) });
			}
			for (std::size_t i = 0; i + 1 < ring_vertices.size(); ++i) {
				for (std::uint32_t s = 0; s < segments; ++s) {
					const std::uint32_t next = (s + 1) % segments;
					mesh.add_face({ at(ring_vertices[i][s], u_at(s), v_at(i)), at(ring_vertices[i][next], u_at(s + 1), v_at(i)),
						at(ring_vertices[i + 1][next], u_at(s + 1), v_at(i + 1)), at(ring_vertices[i + 1][s], u_at(s), v_at(i + 1)) });
				}
			}
			for (std::uint32_t s = 0; s < segments; ++s) {
				const std::uint32_t next = (s + 1) % segments;
				mesh.add_face({ at(bottom, (static_cast<float>(s) + 0.5f) / segment_count, 1.f),
					at(ring_vertices.back()[s], u_at(s), v_at(ring_vertices.size() - 1)),
					at(ring_vertices.back()[next], u_at(s + 1), v_at(ring_vertices.size() - 1)) });
			}
			return mesh;
		}

		shape_result make(const box_params& p)
		{
			if (!positive(p.size.x) || !positive(p.size.y) || !positive(p.size.z)) {
				return { std::nullopt, "box: every size component must be greater than 0 and at most 1e6" };
			}
			poly_mesh mesh;
			const glm::vec3 h = p.size * 0.5f;
			const vertex_id v[8] = {
				mesh.add_vertex({ -h.x, -h.y, -h.z }), mesh.add_vertex({ h.x, -h.y, -h.z }),
				mesh.add_vertex({ h.x, h.y, -h.z }), mesh.add_vertex({ -h.x, h.y, -h.z }),
				mesh.add_vertex({ -h.x, -h.y, h.z }), mesh.add_vertex({ h.x, -h.y, h.z }),
				mesh.add_vertex({ h.x, h.y, h.z }), mesh.add_vertex({ -h.x, h.y, h.z }) };
			constexpr int quads[6][4] = { { 4, 5, 6, 7 }, { 1, 0, 3, 2 }, { 5, 1, 2, 6 }, { 0, 4, 7, 3 }, { 3, 7, 6, 2 }, { 0, 1, 5, 4 } };
			for (const auto& q : quads) {
				mesh.add_face({ at(v[q[0]], 0.f, 0.f), at(v[q[1]], 1.f, 0.f), at(v[q[2]], 1.f, 1.f), at(v[q[3]], 0.f, 1.f) });
			}
			return { std::move(mesh), {} };
		}

		shape_result make(const plane_params& p)
		{
			if (!positive(p.size.x) || !positive(p.size.y)) {
				return { std::nullopt, "plane: both size components must be greater than 0 and at most 1e6" };
			}
			if (!in_range(p.segments_x, 1) || !in_range(p.segments_z, 1)) {
				return { std::nullopt, std::format("plane: segments must be from 1 to {}", max_shape_segments) };
			}
			poly_mesh mesh;
			std::vector<vertex_id> grid;
			grid.reserve(static_cast<std::size_t>(p.segments_x + 1) * (p.segments_z + 1));
			for (std::uint32_t j = 0; j <= p.segments_z; ++j) {
				for (std::uint32_t i = 0; i <= p.segments_x; ++i) {
					const float x = (static_cast<float>(i) / static_cast<float>(p.segments_x) - 0.5f) * p.size.x;
					const float z = (static_cast<float>(j) / static_cast<float>(p.segments_z) - 0.5f) * p.size.y;
					grid.push_back(mesh.add_vertex({ x, 0.f, z }));
				}
			}
			const auto index = [&](std::uint32_t i, std::uint32_t j) { return grid[static_cast<std::size_t>(j) * (p.segments_x + 1) + i]; };
			const auto u = [&](std::uint32_t i) { return static_cast<float>(i) / static_cast<float>(p.segments_x); };
			const auto w = [&](std::uint32_t j) { return static_cast<float>(j) / static_cast<float>(p.segments_z); };
			for (std::uint32_t j = 0; j < p.segments_z; ++j) {
				for (std::uint32_t i = 0; i < p.segments_x; ++i) {
					mesh.add_face({ at(index(i, j), u(i), w(j)), at(index(i, j + 1), u(i), w(j + 1)),
						at(index(i + 1, j + 1), u(i + 1), w(j + 1)), at(index(i + 1, j), u(i + 1), w(j)) });
				}
			}
			return { std::move(mesh), {} };
		}

		shape_result make(const sphere_params& p)
		{
			if (!positive(p.radius)) {
				return { std::nullopt, "sphere: radius must be greater than 0 and at most 1e6" };
			}
			if (!in_range(p.segments, 3) || !in_range(p.rings, 2)) {
				return { std::nullopt, std::format("sphere: segments must be from 3 and rings from 2, both up to {}", max_shape_segments) };
			}
			std::vector<ring> rings;
			rings.reserve(p.rings - 1);
			for (std::uint32_t r = 1; r < p.rings; ++r) {
				const float theta = std::numbers::pi_v<float> * static_cast<float>(r) / static_cast<float>(p.rings);
				rings.push_back({ p.radius * std::sin(theta), p.radius * std::cos(theta) });
			}
			return { revolve(p.radius, rings, -p.radius, p.segments), {} };
		}

		shape_result make(const cylinder_params& p)
		{
			if (!positive(p.radius) || !positive(p.height)) {
				return { std::nullopt, "cylinder: radius and height must be greater than 0 and at most 1e6" };
			}
			if (!in_range(p.segments, 3)) {
				return { std::nullopt, std::format("cylinder: segments must be from 3 to {}", max_shape_segments) };
			}
			poly_mesh mesh;
			std::vector<vertex_id> upper;
			std::vector<vertex_id> lower;
			for (std::uint32_t s = 0; s < p.segments; ++s) {
				const float phi = two_pi * static_cast<float>(s) / static_cast<float>(p.segments);
				const float x = p.radius * std::cos(phi);
				const float z = p.radius * std::sin(phi);
				upper.push_back(mesh.add_vertex({ x, p.height * 0.5f, z }));
				lower.push_back(mesh.add_vertex({ x, -p.height * 0.5f, z }));
			}
			const float segment_count = static_cast<float>(p.segments);
			const auto u_at = [&](std::uint32_t s) { return static_cast<float>(s) / segment_count; };
			for (std::uint32_t s = 0; s < p.segments; ++s) {
				const std::uint32_t next = (s + 1) % p.segments;
				mesh.add_face({ at(upper[s], u_at(s), 1.f), at(upper[next], u_at(s + 1), 1.f),
					at(lower[next], u_at(s + 1), 0.f), at(lower[s], u_at(s), 0.f) });
			}
			const auto disc_uv = [&](std::uint32_t s) {
				const float phi = two_pi * static_cast<float>(s) / segment_count;
				return glm::vec2{ 0.5f + 0.5f * std::cos(phi), 0.5f + 0.5f * std::sin(phi) };
			};
			std::vector<corner> top_cap;
			std::vector<corner> bottom_cap;
			for (std::uint32_t s = 0; s < p.segments; ++s) {
				const std::uint32_t mirrored = p.segments - 1 - s;
				top_cap.push_back({ upper[mirrored], disc_uv(mirrored) });
				bottom_cap.push_back({ lower[s], disc_uv(s) });
			}
			mesh.add_face(std::move(top_cap));
			mesh.add_face(std::move(bottom_cap));
			return { std::move(mesh), {} };
		}

		shape_result make(const capsule_params& p)
		{
			if (!positive(p.radius) || !positive(p.height)) {
				return { std::nullopt, "capsule: radius and height must be greater than 0 and at most 1e6" };
			}
			if (!in_range(p.segments, 3) || !in_range(p.hemisphere_rings, 1)) {
				return { std::nullopt, std::format("capsule: segments must be from 3 and hemisphere_rings from 1, both up to {}", max_shape_segments) };
			}
			const float half = p.height * 0.5f;
			const float quarter = std::numbers::pi_v<float> * 0.5f;
			std::vector<ring> rings;
			rings.reserve(static_cast<std::size_t>(p.hemisphere_rings) * 2);
			for (std::uint32_t k = 1; k <= p.hemisphere_rings; ++k) {
				const float theta = quarter * static_cast<float>(k) / static_cast<float>(p.hemisphere_rings);
				rings.push_back({ p.radius * std::sin(theta), half + p.radius * std::cos(theta) });
			}
			for (std::uint32_t k = 0; k < p.hemisphere_rings; ++k) {
				const float theta = quarter + quarter * static_cast<float>(k) / static_cast<float>(p.hemisphere_rings);
				rings.push_back({ p.radius * std::sin(theta), -half + p.radius * std::cos(theta) });
			}
			return { revolve(half + p.radius, rings, -half - p.radius, p.segments), {} };
		}
	}

	shape_result make_shape(const shape_params& params)
	{
		return std::visit([](const auto& p) { return make(p); }, params);
	}

	std::string_view shape_kind_name(const shape_params& params)
	{
		struct names
		{
			std::string_view operator()(const box_params&) const { return "box"; }
			std::string_view operator()(const plane_params&) const { return "plane"; }
			std::string_view operator()(const sphere_params&) const { return "sphere"; }
			std::string_view operator()(const cylinder_params&) const { return "cylinder"; }
			std::string_view operator()(const capsule_params&) const { return "capsule"; }
		};
		return std::visit(names{}, params);
	}

	std::optional<shape_params> default_shape_params(std::string_view kind)
	{
		if (kind == "box") return box_params{};
		if (kind == "plane") return plane_params{};
		if (kind == "sphere") return sphere_params{};
		if (kind == "cylinder") return cylinder_params{};
		if (kind == "capsule") return capsule_params{};
		return std::nullopt;
	}
}
