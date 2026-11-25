#pragma once
#include "ds_aabb3.hpp"
#include "ds_intersect.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <glm/glm.hpp>

// Pure shape math (EPIC-81, #1328): spheres, capsules and oriented boxes against triangles, and a sphere swept along a
// direction against a triangle. No ECS, no render, no allocation. Everything is in one space (the caller's); touching counts
// as meeting. Distances of a sweep are in units of the direction vector, so a unit direction gives world distances.
namespace ds {
	struct sphere3 final
	{
		glm::vec3 center{ 0.f };
		float radius = 0.f;
	};

	// The segment from `a` to `b` grown by `radius`.
	struct capsule3 final
	{
		glm::vec3 a{ 0.f };
		glm::vec3 b{ 0.f };
		float radius = 0.f;
	};

	// `axes` holds the box's three unit axes as columns, orthogonal to each other; `half` is the half size along each.
	struct obb3 final
	{
		glm::vec3 center{ 0.f };
		glm::vec3 half{ 0.5f };
		glm::mat3 axes{ 1.f };
	};

	inline aabb3 bounds_of(const sphere3& shape)
	{
		const glm::vec3 grow{ shape.radius };
		return { shape.center - grow, shape.center + grow };
	}

	inline aabb3 bounds_of(const capsule3& shape)
	{
		const glm::vec3 grow{ shape.radius };
		return { glm::min(shape.a, shape.b) - grow, glm::max(shape.a, shape.b) + grow };
	}

	inline aabb3 bounds_of(const obb3& shape)
	{
		glm::vec3 extent{ 0.f };
		for (int axis = 0; axis < 3; ++axis) extent += glm::abs(shape.axes[axis]) * shape.half[axis];
		return { shape.center - extent, shape.center + extent };
	}

	namespace shape_detail {
		inline float distance_sq(const glm::vec3& a, const glm::vec3& b)
		{
			const glm::vec3 d = a - b;
			return glm::dot(d, d);
		}

		// n / d, 0 for a zero divisor (no division by a constant zero is ever left in the code, #1350).
		inline float divide(float n, float d) { return d != 0.f ? n / (d != 0.f ? d : 1.f) : 0.f; }

		inline float clamp01(float x) { return std::clamp(x, 0.f, 1.f); }

		// Below this a squared length is no length (an axis that is no axis, a segment that is a point).
		constexpr float k_tiny = 1e-20f;
	}

	// The point of the segment a-b nearest to `p` (a segment that is a point gives that point).
	inline glm::vec3 closest_point_on_segment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
	{
		const glm::vec3 ab = b - a;
		return a + ab * shape_detail::clamp01(shape_detail::divide(glm::dot(p - a, ab), glm::dot(ab, ab)));
	}

	// The point of the triangle nearest to `p` (Ericson, Real-Time Collision Detection 5.1.5).
	inline glm::vec3 closest_point_on_triangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
	{
		using shape_detail::divide;
		const glm::vec3 ab = b - a;
		const glm::vec3 ac = c - a;
		const glm::vec3 ap = p - a;
		const float d1 = glm::dot(ab, ap);
		const float d2 = glm::dot(ac, ap);
		if (d1 <= 0.f && d2 <= 0.f) return a;
		const glm::vec3 bp = p - b;
		const float d3 = glm::dot(ab, bp);
		const float d4 = glm::dot(ac, bp);
		if (d3 >= 0.f && d4 <= d3) return b;
		const float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) return a + ab * divide(d1, d1 - d3);
		const glm::vec3 cp = p - c;
		const float d5 = glm::dot(ab, cp);
		const float d6 = glm::dot(ac, cp);
		if (d6 >= 0.f && d5 <= d6) return c;
		const float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) return a + ac * divide(d2, d2 - d6);
		const float va = d3 * d6 - d5 * d4;
		if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) return b + (c - b) * divide(d4 - d3, (d4 - d3) + (d5 - d6));
		const float denom = va + vb + vc;
		const float v = divide(vb, denom);
		const float w = divide(vc, denom);
		// A triangle that is a line or a point up to rounding has its regions decided by noise: the coordinates leave the
		// triangle or land anywhere in it. The nearest point is then on one of its edges (the sliver's width is lost: at most
		// 1e-4 of its length).
		const glm::vec3 normal = glm::cross(ab, ac);
		const bool sliver = glm::dot(normal, normal) <= 1e-8f * glm::dot(ab, ab) * glm::dot(ac, ac);
		if (sliver || !(denom > 0.f) || v < 0.f || w < 0.f || v + w > 1.f) {
			const glm::vec3 on_ab = closest_point_on_segment(p, a, b);
			const glm::vec3 on_bc = closest_point_on_segment(p, b, c);
			const glm::vec3 on_ca = closest_point_on_segment(p, c, a);
			glm::vec3 best = on_ab;
			float best_sq = shape_detail::distance_sq(p, on_ab);
			for (const glm::vec3& candidate : { on_bc, on_ca }) {
				const float candidate_sq = shape_detail::distance_sq(p, candidate);
				if (candidate_sq < best_sq) {
					best_sq = candidate_sq;
					best = candidate;
				}
			}
			return best;
		}
		return a + ab * v + ac * w;
	}

	// The squared distance between two segments (Ericson 5.1.9).
	inline float segment_segment_distance_sq(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2)
	{
		using namespace shape_detail;
		const glm::vec3 d1 = q1 - p1;
		const glm::vec3 d2 = q2 - p2;
		const glm::vec3 r = p1 - p2;
		const float a = glm::dot(d1, d1);
		const float e = glm::dot(d2, d2);
		const float f = glm::dot(d2, r);
		float s = 0.f;
		float t = 0.f;
		if (a <= k_tiny && e <= k_tiny) {
			s = 0.f;
			t = 0.f;
		} else if (a <= k_tiny) {
			t = clamp01(divide(f, e));
		} else {
			const float c = glm::dot(d1, r);
			if (e <= k_tiny) {
				s = clamp01(divide(-c, a));
			} else {
				const float b = glm::dot(d1, d2);
				const float denom = a * e - b * b;
				s = denom > k_tiny ? clamp01(divide(b * f - c * e, denom)) : 0.f;
				t = divide(b * s + f, e);
				if (t < 0.f) {
					t = 0.f;
					s = clamp01(divide(-c, a));
				} else if (t > 1.f) {
					t = 1.f;
					s = clamp01(divide(b - c, a));
				}
			}
		}
		return shape_detail::distance_sq(p1 + d1 * s, p2 + d2 * t);
	}

	// The squared distance between a segment and a triangle: 0 when they meet, else the least of the segment's ends against
	// the triangle and the segment against each edge.
	inline float segment_triangle_distance_sq(const glm::vec3& p, const glm::vec3& q, const glm::vec3& a, const glm::vec3& b,
		const glm::vec3& c)
	{
		// NaN or infinity: no distance (ray_triangle would call a NaN a hit).
		if (!std::isfinite(glm::dot(p + q + a + b + c, glm::vec3{ 1.f }))) return std::numeric_limits<float>::infinity();
		if (ray_triangle(p, q - p, a, b, c, false, 1.f)) return 0.f;
		float best = std::min(shape_detail::distance_sq(p, closest_point_on_triangle(p, a, b, c)), shape_detail::distance_sq(q, closest_point_on_triangle(q, a, b, c)));
		best = std::min(best, segment_segment_distance_sq(p, q, a, b));
		best = std::min(best, segment_segment_distance_sq(p, q, b, c));
		return std::min(best, segment_segment_distance_sq(p, q, c, a));
	}

	// Whether a triangle meets the box centred at the origin with the given half sizes (separating axis test of Akenine-Moeller:
	// the three box axes, the triangle's normal and the nine edge cross products).
	inline bool triangle_overlaps_box(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& half)
	{
		// NaN or infinity anywhere: no overlap (the comparisons below would answer "not separated" for a NaN).
		if (!std::isfinite(glm::dot(v0 + v1 + v2 + half, glm::vec3{ 1.f }))) return false;
		const glm::vec3 v[3] = { v0, v1, v2 };
		const glm::vec3 edges[3] = { v1 - v0, v2 - v1, v0 - v2 };
		for (int axis = 0; axis < 3; ++axis) {
			const float lo = std::min({ v0[axis], v1[axis], v2[axis] });
			const float hi = std::max({ v0[axis], v1[axis], v2[axis] });
			if (lo > half[axis] || hi < -half[axis]) return false;
		}
		const auto separated_by = [&](const glm::vec3& axis) {
			const float p0 = glm::dot(axis, v[0]);
			const float p1 = glm::dot(axis, v[1]);
			const float p2 = glm::dot(axis, v[2]);
			const float reach = half.x * std::abs(axis.x) + half.y * std::abs(axis.y) + half.z * std::abs(axis.z);
			return std::min({ p0, p1, p2 }) > reach || std::max({ p0, p1, p2 }) < -reach;
		};
		if (separated_by(glm::cross(edges[0], edges[1]))) return false;
		for (int box_axis = 0; box_axis < 3; ++box_axis) {
			glm::vec3 unit{ 0.f };
			unit[box_axis] = 1.f;
			for (const glm::vec3& edge : edges) {
				const glm::vec3 axis = glm::cross(unit, edge);
				if (glm::dot(axis, axis) <= shape_detail::k_tiny) continue;
				if (separated_by(axis)) return false;
			}
		}
		return true;
	}

	inline bool sphere_triangle(const sphere3& shape, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
	{
		return shape_detail::distance_sq(shape.center, closest_point_on_triangle(shape.center, a, b, c)) <= shape.radius * shape.radius;
	}

	inline bool capsule_triangle(const capsule3& shape, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
	{
		return segment_triangle_distance_sq(shape.a, shape.b, a, b, c) <= shape.radius * shape.radius;
	}

	// Whether the three columns are unit vectors at right angles (within rounding).
	inline bool orthonormal(const glm::mat3& axes)
	{
		constexpr float k_slack = 1e-3f;
		for (int i = 0; i < 3; ++i) {
			if (std::abs(glm::dot(axes[i], axes[i]) - 1.f) > k_slack) return false;
			for (int j = i + 1; j < 3; ++j) {
				if (std::abs(glm::dot(axes[i], axes[j])) > k_slack) return false;
			}
		}
		return true;
	}

	inline bool obb_triangle(const obb3& shape, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
	{
		ASSERT_MSG(!std::isfinite(glm::dot(shape.axes[0] + shape.axes[1] + shape.axes[2], glm::vec3{ 1.f })) || orthonormal(shape.axes),
			"obb_triangle: the box's axes are not unit vectors at right angles");
		const glm::mat3 to_local = glm::transpose(shape.axes);
		return triangle_overlaps_box(to_local * (a - shape.center), to_local * (b - shape.center), to_local * (c - shape.center), shape.half);
	}

	// A sphere that moved `t` along its direction and met a triangle.
	struct sweep_hit final
	{
		float t = 0.f;
		// The point of the triangle the sphere touches, and the unit direction from it to the sphere's centre at `t`.
		glm::vec3 point{ 0.f };
		glm::vec3 normal{ 0.f };
		// The sphere met the triangle where it started (touching counts, also when it moves away): t is 0 and `normal` is the way
		// out, or against the motion when the centre is on the triangle.
		bool started_inside = false;
	};

	// The first time the sphere, moving along `direction`, touches the triangle within [0, t_max]: its face (the plane moved by
	// the radius), its edges (a cylinder around each) and its corners (a sphere around each), the least of the three.
	inline std::optional<sweep_hit> sphere_sweep_triangle(const sphere3& shape, const glm::vec3& direction, const glm::vec3& a,
		const glm::vec3& b, const glm::vec3& c, float t_max = std::numeric_limits<float>::infinity())
	{
		using namespace shape_detail;
		const float radius_sq = shape.radius * shape.radius;
		const glm::vec3 triangle_normal = glm::cross(b - a, c - a);
		const float normal_length = glm::length(triangle_normal);
		const float speed_sq = glm::dot(direction, direction);
		if (!std::isfinite(speed_sq)) return std::nullopt;

		const glm::vec3 start_point = closest_point_on_triangle(shape.center, a, b, c);
		if (shape_detail::distance_sq(shape.center, start_point) <= radius_sq) {
			sweep_hit hit;
			hit.started_inside = true;
			hit.point = start_point;
			const glm::vec3 out = shape.center - start_point;
			const float out_length = glm::length(out);
			if (out_length > 0.f) {
				hit.normal = out / out_length;
			} else if (normal_length > 0.f) {
				const glm::vec3 unit = triangle_normal / normal_length;
				hit.normal = glm::dot(unit, direction) > 0.f ? -unit : unit;
			} else {
				hit.normal = speed_sq > 0.f ? -direction / std::sqrt(speed_sq) : glm::vec3{ 0.f, 0.f, 1.f };
			}
			return hit;
		}
		if (speed_sq <= 0.f) return std::nullopt;

		float best = t_max;
		bool found = false;
		const auto take = [&](float t) {
			if (t >= 0.f && t <= best) {
				best = t;
				found = true;
			}
		};

		if (normal_length > 0.f) {
			const glm::vec3 unit = triangle_normal / normal_length;
			const float approach = glm::dot(direction, unit);
			const float offset = glm::dot(shape.center - a, unit);
			const float side = offset >= 0.f ? 1.f : -1.f;
			if (side * approach < 0.f) {
				const float t = (side * shape.radius - offset) / approach;
				if (t >= 0.f && t <= best) {
					const glm::vec3 contact = shape.center + direction * t - unit * (side * shape.radius);
					const glm::vec3 ab = b - a;
					const glm::vec3 ac = c - a;
					const glm::vec3 ap = contact - a;
					const float d00 = glm::dot(ab, ab);
					const float d01 = glm::dot(ab, ac);
					const float d11 = glm::dot(ac, ac);
					const float d20 = glm::dot(ap, ab);
					const float d21 = glm::dot(ap, ac);
					const float denom = d00 * d11 - d01 * d01;
					if (denom > 0.f) {
						const float v = (d11 * d20 - d01 * d21) / denom;
						const float w = (d00 * d21 - d01 * d20) / denom;
						constexpr float k_edge_slack = -1e-6f;
						if (v >= k_edge_slack && w >= k_edge_slack && 1.f - v - w >= k_edge_slack) take(t);
					}
				}
			}
		}

		const auto sweep_edge = [&](const glm::vec3& p0, const glm::vec3& p1) {
			const glm::vec3 edge = p1 - p0;
			const float edge_sq = glm::dot(edge, edge);
			if (edge_sq <= 0.f) return;
			const glm::vec3 m = shape.center - p0;
			const glm::vec3 along = direction - edge * (glm::dot(direction, edge) / edge_sq);
			const glm::vec3 from = m - edge * (glm::dot(m, edge) / edge_sq);
			const float qa = glm::dot(along, along);
			// The motion along the edge's own direction: nothing to solve when that is all there is.
			if (qa <= 1e-12f * speed_sq) return;
			const float qb = glm::dot(from, along);
			const float qc = glm::dot(from, from) - radius_sq;
			const float disc = qb * qb - qa * qc;
			if (disc < 0.f) return;
			const float t = (-qb - std::sqrt(disc)) / qa;
			if (t < 0.f || t > best) return;
			const float s = glm::dot(m + direction * t, edge) / edge_sq;
			if (s >= 0.f && s <= 1.f) take(t);
		};
		sweep_edge(a, b);
		sweep_edge(b, c);
		sweep_edge(c, a);

		for (const glm::vec3& corner : { a, b, c }) {
			const glm::vec3 m = shape.center - corner;
			const float qb = glm::dot(m, direction);
			const float qc = glm::dot(m, m) - radius_sq;
			const float disc = qb * qb - speed_sq * qc;
			if (disc < 0.f) continue;
			take((-qb - std::sqrt(disc)) / speed_sq);
		}

		if (!found) return std::nullopt;
		sweep_hit hit;
		hit.t = best;
		const glm::vec3 at = shape.center + direction * best;
		hit.point = closest_point_on_triangle(at, a, b, c);
		const glm::vec3 out = at - hit.point;
		const float out_length = glm::length(out);
		if (out_length > 0.f) {
			hit.normal = out / out_length;
		} else if (normal_length > 0.f) {
			const glm::vec3 unit = triangle_normal / normal_length;
			hit.normal = glm::dot(unit, direction) > 0.f ? -unit : unit;
		} else {
			hit.normal = -direction / std::sqrt(speed_sq);
		}
		return hit;
	}
}
