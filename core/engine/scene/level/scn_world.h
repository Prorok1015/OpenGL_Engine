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

		void mark_systems_graphs_dirty() {
			m_fixed_graph = m_fixed_organizer.graph();
			m_variable_graph = m_organizer.graph();
		}
	private:
		void run_graph(std::vector<entt::organizer::vertex>& graph) {
			for (auto&& node : graph) {
				node.prepare(m_state);
				node.callback()(node.data(), m_state);
			}
		}

	private:
		std::string name;
		float m_accumulator = 0.0f;
		const float m_fixed_step = 1.0f / 60.0f; // 60 Hz

		entt::registry m_state;
		entt::organizer m_organizer;
		entt::organizer m_fixed_organizer;
		std::vector<entt::organizer::vertex> m_fixed_graph;
		std::vector<entt::organizer::vertex> m_variable_graph;
		std::vector<std::unique_ptr<ecs::system_interface>> m_systems;
	};
}