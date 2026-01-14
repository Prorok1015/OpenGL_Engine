#pragma once
#include "scn_level.h"

namespace scn
{
	class level_manager
	{
	public:
		level_manager() = default;
		~level_manager() = default;
		level_manager(const level_manager&) = delete;
		level_manager(level_manager&&) = default;
		level_manager& operator=(const level_manager&) = delete;
		level_manager& operator=(level_manager&&) = default;

		void update(std::chrono::duration<float> dt);

		scn::level& get_level() { return active_lvl; }

	private:
		scn::level active_lvl;
	};
}