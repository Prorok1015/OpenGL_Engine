#pragma once
#include <snk/ecs/ecs_entt_abi.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <entt/entity/registry.hpp>
#include <string_view>

namespace ecs {

// What a bus subscriber gets during a delivery (WORLD_BUS.md 3.4). The world is read-only to it: structural edits go
// through changer()/spawner() and land in one apply_commands right after the delivery's subscribers; what it emits goes
// to the next delivery; mail to other worlds of the set goes through outbox().
class bus_context
{
public:
	bus_context(std::string_view set, std::string_view world, entt::registry& registry)
		: m_set(set), m_world(world), m_registry(registry)
	{
	}

	std::string_view set() const { return m_set; }
	std::string_view world() const { return m_world; }
	const entt::registry& registry() const { return m_registry; }

	entity_changer& changer() { return m_registry.ctx().emplace<entity_changer>(); }
	entity_spawner& spawner() { return m_registry.ctx().emplace<entity_spawner>(); }

	// Unbound when T is not an event type of the world: emit then asserts.
	template <class T>
	event_writer<T>& writer() { return m_registry.ctx().emplace<event_writer<T>>(); }

	// Unbound when T is not a message type of the set: send then asserts.
	template <class T>
	ecs::outbox<T>& outbox() { return m_registry.ctx().emplace<ecs::outbox<T>>(); }

private:
	std::string_view m_set;
	std::string_view m_world;
	entt::registry& m_registry;
};

} // namespace ecs
