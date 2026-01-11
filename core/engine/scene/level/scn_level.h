#pragma once
#include "scn_world.h"
#include <unordered_map>

namespace scn
{
	class level
	{
	public:
		level() = default;
		~level() = default;
		level(const level&) = delete;
		level(level&&) = default;
		level& operator=(const level&) = delete;
		level& operator=(level&&) = default;

		void update(std::chrono::duration<float> dt);

		void add_world(const std::string& type, scn::world&& world) {
			m_worlds[type] = std::make_unique<scn::world>(std::move(world));
		}

		scn::world& get_world(const std::string& type) {
			ASSERT_MSG(m_worlds.contains(type), "level doesn't contain this type of world");
			return *m_worlds[type];
		}
	private:
		std::unordered_map<std::string, std::unique_ptr<scn::world>> m_worlds;
	};
}