#pragma once
#include <glm/glm.hpp>
#include <limits>

namespace ds {
	using point2d = glm::vec2;

	struct bbox final
	{
		point2d min;
		point2d max;

		bbox(point2d mi, point2d mx)
			: min{ mi }
			, max{ mx }
		{
		}

		bbox()
			: min{ std::numeric_limits<float>::max() }
			, max{ std::numeric_limits<float>::lowest() }
		{
		}

		bbox(const bbox&) = default;
		bbox& operator=(const bbox&) = default;
		bbox(bbox&&) = default;
		bbox& operator=(bbox&&) = default;

		friend auto operator==(const bbox& a, const bbox& b) {
			return a.min == b.min && a.max == b.max;
		}
		friend bool operator!=(const bbox& a, const bbox& b) = default;
	};

	inline point2d center(const bbox& box) { return (box.min + box.max) * 0.5f; }
	inline void expand(bbox& box, const bbox& other) 
	{
		box.min = glm::min(box.min, other.min);
		box.max = glm::max(box.max, other.max);
	}

	inline void expand(bbox& box, const point2d& p) 
	{
		box.min = glm::min(box.min, p);
		box.max = glm::max(box.max, p);
	}

	inline bool intersects(const bbox& a, const bbox& b) 
	{
		return (a.min.x <= b.max.x && a.max.x >= b.min.x) && (a.min.y <= b.max.y && a.max.y >= b.min.y);
	}

	inline bool contains(const bbox& a, const point2d& p) 
	{
		return p.x >= a.min.x && p.x <= a.max.x && p.y >= a.min.y && p.y <= a.max.y;
	}

	inline double area(const bbox& box) 
	{
		auto ext = box.max - box.min;
		return static_cast<double>(ext.x) * static_cast<double>(ext.y);
	}
}