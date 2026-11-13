#pragma once
#include <snk/ecs/ecs_entt_abi.h>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <entt/entity/registry.hpp>
#include <entt/entity/snapshot.hpp>
namespace ecs
{
	enum class sandbox_entity : std::uint32_t {};
	using sandbox_registry = entt::basic_registry<sandbox_entity>;
	using sandbox_snapshot = entt::basic_snapshot<sandbox_registry>;
	using sandbox_continuos_loader = entt::basic_continuous_loader<sandbox_registry>;
	using sandbox_snapshot_loader = entt::basic_snapshot_loader<sandbox_registry>;

	class entity_spawner : public sandbox_registry
	{
	};
}