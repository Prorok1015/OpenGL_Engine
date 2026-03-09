#pragma once
#include "ecs_system.h"
#include "ecs_system_interface.hpp"
#include "scn_level_desc.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <chrono>

namespace scn
{
	class world
	{
	public:
		world(uint32_t id, std::string_view name)
			: m_id(id), m_name(name) {
		}
		~world() = default;
		world& operator=(const world&) = default;
		world& operator=(world&&) = default;
		world(const world&) = default;
		world(world&&) = default;

		entt::registry& state() { return m_state; }
		entt::registry const& state() const { return m_state; }
		uint32_t get_id() const { return m_id; }
		const std::string_view get_name() const { return m_name; }
	private:
		uint32_t m_id;
		std::string m_name;
		entt::registry m_state;
		std::vector<std::unique_ptr<ecs::system_interface>> m_systems;
	};
}