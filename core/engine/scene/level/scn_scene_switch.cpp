#include <snk/scene/level/scn_scene_switch.h>
#include "scn_level_switch_slot.h"
#include <snk/common/engine_assert.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/scene/level/scn_context_set.h>
#include <entt/entt.hpp>
#include <utility>

namespace {
	// The set running now owns the world when it is one of its worlds; outside a set's update the world's ctx says.
	scn::context_set* set_of(const entt::registry& world)
	{
		if (scn::context_set* running = scn::context_set::current()) {
			bool owns = false;
			running->for_each([&](scn::world_context& context) { owns = owns || &context.state() == &world; });
			if (owns) return running;
		}
		const auto* port = world.ctx().find<scn::detail::level_switch_port>();
		return port ? port->set : nullptr;
	}
}

bool scn::request_level(const entt::registry& world, const res::tag& level)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "request_level: main thread only (a task asks from its then_main continuation)");
	context_set* set = set_of(world);
	if (!set) {
		egLOG_WARN("scene", "request_level '{}': the world belongs to no set", level.view());
		return false;
	}
	auto& slot = set->level_switch();
	if (slot.closed) {
		if (!std::exchange(slot.closed_said, true)) {
			egLOG_WARN("scene", "request_level '{}': the set '{}' takes no level switch here (it is the edited level)", level.view(), set->name());
		}
		return false;
	}
	slot.status = level_switch_status::loading;
	slot.level = level;
	slot.loading = res::res_handle<scn::level_desc>{};
	return true;
}

scn::level_switch_state scn::level_switch(const entt::registry& world)
{
	const context_set* set = set_of(world);
	if (!set) return {};
	const auto& slot = set->level_switch();
	return { .status = slot.status, .level = slot.status == level_switch_status::idle ? res::tag{} : slot.level };
}
