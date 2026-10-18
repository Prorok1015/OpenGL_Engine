#pragma once
#include <snk/resource/res_tag.h>
#include "level/scn_world_desc.h"
#include <cstdint>
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
		// __guid of the level file in level_tag; empty until the first save (or when loaded from a file without one).
		std::string level_guid;
		bool is_dirty = false;

		// Multi-world state
		std::vector<scn::world_desc> world_descs;
		std::vector<std::string> world_names;
		std::vector<std::vector<std::string>> worlds_systems_list;
		// -1 (or any index outside world_descs) = the level has no worlds; use the accessors below, never index directly.
		int active_world_idx = 0;
		// Bumped on every change of the edited descs (and of which one is edited); caches of derived views key on it.
		uint64_t desc_generation = 0;
		void touch_descs() { ++desc_generation; }

		bool has_active_world() const { return active_world_idx >= 0 && active_world_idx < static_cast<int>(world_descs.size()); }
		scn::world_desc* active_world_desc() { return has_active_world() ? &world_descs[active_world_idx] : nullptr; }
		const scn::world_desc* active_world_desc() const { return has_active_world() ? &world_descs[active_world_idx] : nullptr; }
		// Name of the active world as the level knows it, null when there is none.
		const std::string* active_world_name() const
		{
			return has_active_world() && active_world_idx < static_cast<int>(world_names.size()) ? &world_names[active_world_idx] : nullptr;
		}
	};
}
