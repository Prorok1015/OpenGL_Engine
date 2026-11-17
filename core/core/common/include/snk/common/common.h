#pragma once
#include <algorithm>
#include <functional>
#include <vector>
#include <array>
#include <queue>
#include <ranges>
#include <string>
#include <numbers>
#include <filesystem>
#include <type_traits>
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <glm/gtx/quaternion.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/ds/ds_event.hpp>
#include <snk/common/ds/ds_polymorphic_cast.hpp>

namespace ds {
	using color = glm::vec4;

	template <typename SIGNATURE>
	using managed_event = Event<SIGNATURE, EventPolicyManagedContainer<EventStoragePopicyVector>>;

	template <typename SIGNATURE = void()>
	using simple_event = Event<SIGNATURE, EventPolicySimpleContainer<EventStoragePopicyVector>>;
}