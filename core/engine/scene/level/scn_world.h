#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <chrono>

namespace scn
{
	class world
	{
		void update(std::chrono::duration<std::chrono::milliseconds> dt);

		entt::registry& state() { return m_state; }
		entt::registry const& state() const { return m_state; }

	private:
		entt::organizer m_organizer;// maybe entt::graph?
		entt::registry m_state;
	};
}