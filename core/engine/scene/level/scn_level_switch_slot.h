#pragma once
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_scene_switch.h>

// What request_level leaves for context_manager::update (#564): not part of the engine's API.
namespace scn
{
	class context_set;

	namespace detail
	{
		// One per set. A request while another loads replaces it: the earlier load's handle is dropped unread.
		struct level_switch_slot
		{
			level_switch_status status = level_switch_status::idle;
			res::tag level;
			res::res_handle<level_desc> loading;
		};

		// In the ctx of every world of a set, refreshed before each frame of the set: request_level from outside a set's
		// update (a task's continuation) finds the set here, whatever rebuilt or restored the world since.
		struct level_switch_port
		{
			context_set* set = nullptr;
		};
	}
}
