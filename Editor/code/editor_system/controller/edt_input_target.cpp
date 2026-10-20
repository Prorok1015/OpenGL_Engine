#include "edt_input_target.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/scene/level/scn_context_manager.h>

std::optional<inp::input_target> edt::active_play_input_target(const editor_services& services, const scene_document& state)
{
	const auto contexts = services.ctx_manager.lock();
	const std::string* name = state.active_world_name();
	if (!contexts || !name || !contexts->find("play", *name)) return std::nullopt;
	return inp::input_target{ .set = "play", .context = *name };
}

std::optional<inp::input_target> edt::active_input_target(const editor_services& services, const scene_document& state)
{
	if (auto play = active_play_input_target(services, state)) return play;

	const auto contexts = services.ctx_manager.lock();
	const std::string* name = state.active_world_name();
	if (!contexts || !name) return std::nullopt;
	if (contexts->find(scn::context_manager::level_set_name, *name)) {
		return inp::input_target{ .set = std::string{ scn::context_manager::level_set_name }, .context = *name };
	}
	return std::nullopt;
}
