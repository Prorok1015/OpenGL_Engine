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
#include "eng_performance_timer.hpp"
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
}
