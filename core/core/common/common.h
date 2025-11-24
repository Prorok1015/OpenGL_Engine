#pragma once
#include <iostream>
#include <algorithm>
#include <functional>
#include <vector>
#include <array>
#include <queue>
#include <ranges>
#include <string>
#include <iomanip>
#include <numbers>
#include <filesystem>
#include <type_traits>
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <glm/gtx/quaternion.hpp>
#include "engine_assert.h"

#include "ds_event.hpp"

using namespace std::string_literals;
using namespace std::string_view_literals;

template <typename SIGNATURE>
using EventManaged = ds::Event<SIGNATURE, ds::EventPolicyManagedContainer<ds::EventStoragePopicyVector>>;

template <typename SIGNATURE = void()>
using Event = ds::Event<SIGNATURE, ds::EventPolicySimpleContainer<ds::EventStoragePopicyVector>>;

namespace ds
{
	using color = glm::vec4;

	template<typename T, typename U>
	T polymorphic_cast(U ptr)
	{
		auto ptr1 = static_cast<T>(ptr);
		auto ptr2 = dynamic_cast<T>(ptr);
		ASSERT_MSG(ptr1 == ptr2, "polymorphic_cast failed");
		return ptr1;
	}

	struct performance_timer
	{
		performance_timer()
		{
			reset();
		}
		void reset()
		{
			start_time = std::chrono::high_resolution_clock::now();
		}
		double elapsed_seconds() const
		{
			auto end_time = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> elapsed = end_time - start_time;
			return elapsed.count();
		}
	private:
		std::chrono::high_resolution_clock::time_point start_time;
	};

	struct scoped_timer
	{
		scoped_timer(const std::string& name)
			: name(name)
		{
			timer.reset();
		}
		~scoped_timer()
		{
			auto elapsed = timer.elapsed_seconds();
			std::cout << "Timer [" << name << "] elapsed time: " << elapsed << " seconds." << std::endl;
		}
	private:
		std::string name;
		performance_timer timer;
	};
}

namespace ds {
	using point2d = glm::vec2;

	struct bbox
	{
		union
		{
			point2d p[2];
			struct { point2d min; point2d max; };
		};

		bbox()
			: min{ std::numeric_limits<float>::max() }
			, max{ std::numeric_limits<float>::lowest() }
		{
		}
	};

	using triangle = uint32_t;
}


namespace ds {

	point2d center(const bbox& box);
	void expand(bbox& box, const bbox& other);
	void expand(bbox& box, const point2d& p);
	bool intersects(const bbox& a, const bbox& b);
	bool contains(const bbox& a, const point2d& p);
	double area(const bbox& box);
}