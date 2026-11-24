#pragma once
#include <glm/glm.hpp>
#include <limits>

namespace ds {
	// A box in 3D space. `ds::bbox` (ds_bbox.hpp) is the 2D one (UV space of a geometry) and stays as it is.
	// The default box is empty (min above max): expanding it by a point makes the box of that point, and an empty box
	// changes nothing it is expanded by.
	struct aabb3 final
	{
		glm::vec3 min{ std::numeric_limits<float>::max() };
		glm::vec3 max{ std::numeric_limits<float>::lowest() };

		aabb3() = default;
		aabb3(const glm::vec3& lo, const glm::vec3& hi) : min(lo), max(hi) {}

		bool empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }

		friend bool operator==(const aabb3& a, const aabb3& b) { return a.min == b.min && a.max == b.max; }
		friend bool operator!=(const aabb3& a, const aabb3& b) { return !(a == b); }
	};

	inline void expand(aabb3& box, const glm::vec3& point)
	{
		box.min = glm::min(box.min, point);
		box.max = glm::max(box.max, point);
	}

	inline void expand(aabb3& box, const aabb3& other)
	{
		if (other.empty()) return;
		box.min = glm::min(box.min, other.min);
		box.max = glm::max(box.max, other.max);
	}

	inline glm::vec3 center(const aabb3& box) { return (box.min + box.max) * 0.5f; }
	inline glm::vec3 size(const aabb3& box) { return box.max - box.min; }

	// Touching boxes intersect. An empty box intersects nothing.
	inline bool intersects(const aabb3& a, const aabb3& b)
	{
		if (a.empty() || b.empty()) return false;
		return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z
			&& a.max.z >= b.min.z;
	}

	inline bool contains(const aabb3& box, const glm::vec3& point)
	{
		return point.x >= box.min.x && point.x <= box.max.x && point.y >= box.min.y && point.y <= box.max.y && point.z >= box.min.z
			&& point.z <= box.max.z;
	}

	// 0 for an empty box.
	inline float surface_area(const aabb3& box)
	{
		if (box.empty()) return 0.f;
		const glm::vec3 d = box.max - box.min;
		return 2.f * (d.x * d.y + d.y * d.z + d.z * d.x);
	}

	// The box of the eight transformed corners (an empty box stays empty).
	inline aabb3 transformed(const aabb3& box, const glm::mat4& matrix)
	{
		if (box.empty()) return {};
		aabb3 out;
		for (int corner = 0; corner < 8; ++corner) {
			const glm::vec3 point{ (corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y,
				(corner & 4) ? box.max.z : box.min.z };
			expand(out, glm::vec3(matrix * glm::vec4(point, 1.f)));
		}
		return out;
	}
}
