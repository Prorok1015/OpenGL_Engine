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
	public:
		world() = default;
		~world() = default;
		world& operator=(const world&) = default;
		world& operator=(world&&) = default;
		world(const world&) = default;
		world(world&&) = default;

		void update(std::chrono::duration<float> dt);

		entt::registry& state() { return m_state; }
		entt::registry const& state() const { return m_state; }

		entt::organizer& organizer() { return m_organizer; }
		entt::organizer const& organizer() const { return m_organizer; }

	private:
		std::string name;
		entt::organizer m_organizer;// maybe entt::graph?
		entt::registry m_state;
	};
}