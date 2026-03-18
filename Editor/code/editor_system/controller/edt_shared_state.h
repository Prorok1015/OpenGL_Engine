#pragma once
#include "res_tag.h"
#include "level/scn_world_desc.h"
#include <vector>
#include <string>
#include <memory>

namespace scn { class level_manager; class ecs_assembler; }
namespace ecs { class system_factory; }

namespace edt
{
	struct shared_state
	{
		// Service references (set during init, not owned)
		std::weak_ptr<scn::level_manager> lvl_manager;
		scn::ecs_assembler* assembler = nullptr;
		ecs::system_factory* sfactory = nullptr;

		// Level identity
		res::tag editor_tag;
		res::tag level_tag;
		std::string level_name;
		bool is_dirty = false;

		// Multi-world state
		std::vector<scn::world_desc> world_descs;
		std::vector<std::string> world_names;
		std::vector<std::vector<std::string>> worlds_systems_list;
		int active_world_idx = 0;

		scn::world_desc& active_world_desc() { return world_descs[active_world_idx]; }
		const scn::world_desc& active_world_desc() const { return world_descs[active_world_idx]; }
	};
}
