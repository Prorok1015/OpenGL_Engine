#pragma once
#include <cstdint>
#include "entt/fwd.hpp"

namespace ecs {
	// +1 on any change to the virtual functions below: the module layout key mixes it in, tools/ci/vtable_pins.json pins them (#922).
	inline constexpr std::uint32_t system_interface_abi_revision = 1;

	class system_interface
	{
	public:
		virtual ~system_interface() = default;
	};
}