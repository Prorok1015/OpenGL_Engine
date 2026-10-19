#include "edt_input_target.h"
#include "edt_shared_state.h"
#include <snk/scene/level/scn_context_manager.h>

std::optional<inp::input_target> edt::active_input_target(const shared_state& state)
{
	const auto contexts = state.ctx_manager.lock();
	const std::string* name = state.active_world_name();
	if (!contexts || !name) return std::nullopt;

	if (contexts->find("play", *name)) {
		return inp::input_target{ .set = "play", .context = *name };
	}
	if (contexts->find(scn::context_manager::level_set_name, *name)) {
		return inp::input_target{ .set = std::string{ scn::context_manager::level_set_name }, .context = *name };
	}
	return std::nullopt;
}
