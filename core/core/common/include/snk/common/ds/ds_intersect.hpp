#pragma once
#include "ds_aabb3.hpp"
#include <snk/common/engine_assert.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <glm/glm.hpp>

// Pure intersection math: no ECS, no render, no allocation. Distances are in units of the ray's direction vector, so a
// unit direction gives world distances.
namespace ds {
	// A ray with the reciprocal of its direction kept for the slab test (an axis the direction does not move along has an
	// infinite reciprocal, which ray_aabb never multiplies with: it checks the origin against that slab instead).
	struct ray3 final
	{
		glm::vec3 origin{ 0.f };
		glm::vec3 direction{ 0.f, 0.f, -1.f };
		glm::vec3 inv_direction{ std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), -1.f };

		ray3() = default;
		ray3(const glm::vec3& from, const glm::vec3& dir)
			: origin(from), direction(dir), inv_direction(1.f / dir.x, 1.f / dir.y, 1.f / dir.z) {}

		glm::vec3 at(float t) const { return origin + direction * t; }
	};

	// The distance at which the ray enters the box (0 when the origin is inside it), or nothing when it misses or enters
	// past `t_max`. Touching a face or an edge is a hit.
	inline std::optional<float> ray_aabb(const ray3& ray, const aabb3& box, float t_max = std::numeric_limits<float>::infinity())
	{
		ASSERT_MSG(!std::isnan(ray.origin.x + ray.origin.y + ray.origin.z + ray.direction.x + ray.direction.y + ray.direction.z),
			"ray_aabb: the ray has a NaN origin or direction");
		if (box.empty()) return std::nullopt;
		float t_near = 0.f;
		float t_far = t_max;
		for (int axis = 0; axis < 3; ++axis) {
			if (!std::isfinite(ray.inv_direction[axis])) {
				// The ray does not move along this axis: it is inside the slab everywhere or nowhere (touching a face counts).
				if (ray.origin[axis] < box.min[axis] || ray.origin[axis] > box.max[axis]) return std::nullopt;
				continue;
			}
			const float t1 = (box.min[axis] - ray.origin[axis]) * ray.inv_direction[axis];
			const float t2 = (box.max[axis] - ray.origin[axis]) * ray.inv_direction[axis];
			t_near = std::max(t_near, std::min(t1, t2));
			t_far = std::min(t_far, std::max(t1, t2));
		}
		if (t_near > t_far) return std::nullopt;
		return t_near;
	}

	struct triangle_hit final
	{
		float t = 0.f;
		// Barycentric coordinates of the hit: point = (1 - u - v) * a + u * b + v * c.
		float u = 0.f;
		float v = 0.f;
		// The ray met the triangle from behind (a counter-clockwise triangle's front is the side its normal b-a x c-a points to).
		bool back_face = false;
	};

	// Moeller-Trumbore. A ray parallel to the triangle's plane misses. `cull_back_faces`: a hit from behind is no hit.
	inline std::optional<triangle_hit> ray_triangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a,
		const glm::vec3& b, const glm::vec3& c, bool cull_back_faces = false, float t_max = std::numeric_limits<float>::infinity())
	{
		// Relative: sin of the angle between the ray and the triangle's plane, so the size of the triangle and of the direction
		// vector do not decide what counts as parallel.
		constexpr float k_parallel = 1e-7f;
		const glm::vec3 edge1 = b - a;
		const glm::vec3 edge2 = c - a;
		const glm::vec3 p = glm::cross(direction, edge2);
		const float det = glm::dot(edge1, p);
		const float limit = k_parallel * glm::length(edge1) * glm::length(p);
		if (cull_back_faces ? det <= limit : std::abs(det) <= limit) return std::nullopt;
		const float inv_det = 1.f / det;
		const glm::vec3 s = origin - a;
		const float u = glm::dot(s, p) * inv_det;
		if (u < 0.f || u > 1.f) return std::nullopt;
		const glm::vec3 q = glm::cross(s, edge1);
		const float v = glm::dot(direction, q) * inv_det;
		if (v < 0.f || u + v > 1.f) return std::nullopt;
		const float t = glm::dot(edge2, q) * inv_det;
		if (t < 0.f || t > t_max) return std::nullopt;
		return triangle_hit{ t, u, v, det < 0.f };
	}

	// Six planes with normals pointing in; a point is inside when dot(normal, point) + d >= 0 for all of them.
	struct frustum final
	{
		std::array<glm::vec4, 6> planes{};
	};

	// The planes of a view-projection matrix (glm, OpenGL clip space: -w <= x, y, z <= w).
	inline frustum frustum_from(const glm::mat4& view_projection)
	{
		const auto row = [&view_projection](int i) {
			return glm::vec4(view_projection[0][i], view_projection[1][i], view_projection[2][i], view_projection[3][i]);
		};
		frustum out;
		out.planes = { row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1), row(3) + row(2), row(3) - row(2) };
		for (glm::vec4& plane : out.planes) {
			const float length = glm::length(glm::vec3(plane));
			if (length > 0.f) plane /= length;
		}
		return out;
	}

	enum class frustum_test : unsigned char { outside, intersects, inside };

	inline frustum_test test_frustum(const frustum& view, const aabb3& box)
	{
		if (box.empty()) return frustum_test::outside;
		bool fully_inside = true;
		for (const glm::vec4& plane : view.planes) {
			const glm::vec3 normal{ plane };
			// The corner furthest along the normal and the one furthest against it.
			const glm::vec3 positive{ normal.x >= 0.f ? box.max.x : box.min.x, normal.y >= 0.f ? box.max.y : box.min.y,
				normal.z >= 0.f ? box.max.z : box.min.z };
			const glm::vec3 negative{ normal.x >= 0.f ? box.min.x : box.max.x, normal.y >= 0.f ? box.min.y : box.max.y,
				normal.z >= 0.f ? box.min.z : box.max.z };
			if (glm::dot(normal, positive) + plane.w < 0.f) return frustum_test::outside;
			if (glm::dot(normal, negative) + plane.w < 0.f) fully_inside = false;
		}
		return fully_inside ? frustum_test::inside : frustum_test::intersects;
	}
}
