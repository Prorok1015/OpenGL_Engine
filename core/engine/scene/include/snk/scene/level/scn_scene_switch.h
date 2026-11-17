#pragma once
#include <snk/common/snk_api.h>
#include <snk/resource/res_tag.h>
#include <entt/entity/fwd.hpp>
#include <cstdint>

namespace scn
{
	enum class level_switch_status : std::uint8_t
	{
		idle,
		// The level desc loads; the current level goes on until it is ready.
		loading,
		// The last requested level did not load: the current level stays.
		failed,
	};

	struct level_switch_state
	{
		level_switch_status status = level_switch_status::idle;
		// The level loading or the one that failed; empty when idle.
		res::tag level;
	};

	// Replaces every world of the set `world` belongs to with the worlds of `level` (#564). The desc loads in the
	// background while the current level goes on; once it is ready, the set is rebuilt at the start of the next frame,
	// before any world of any set runs. A request while another one loads replaces it. Callable from any system, bus
	// subscriber or observer, and from the main thread outside the frame. In the editor's Play only the worlds of Play
	// are replaced, never the edited level. False, with a line in the log, when `world` belongs to no set.
	// Data that must outlive the switch belongs in the module layer's storage: the worlds and the set's blackboard go.
	// Before the worlds go the set's tasks are stopped and waited for (as by a set's destruction): a task of the level
	// must not wait for the main thread (its own then_main, post_main, a resource the main thread finishes), or the
	// switch hangs the frame. A request still loading when the game module is swapped is dropped with the set.
	SNK_API bool request_level(const entt::registry& world, const res::tag& level);

	// Where the last request of the set `world` belongs to stands.
	SNK_API level_switch_state level_switch(const entt::registry& world);
}
