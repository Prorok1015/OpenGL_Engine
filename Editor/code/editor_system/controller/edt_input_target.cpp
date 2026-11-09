#include "edt_input_target.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <string>

scn::context_set* edt::view_set(const editor_services& services)
{
	const auto contexts = services.ctx_manager.lock();
	if (!contexts) return nullptr;
	// An inactive "play" is a restore waiting for its resources (play_session::reloading): nothing to show yet.
	if (scn::context_set* play = contexts->find_set(k_play_set)) return play->active() ? play : nullptr;
	return &contexts->level_set();
}

entt::registry* edt::view_registry(const editor_services& services, const scene_document& state)
{
	scn::context_set* set = view_set(services);
	const std::string* name = state.active_world_name();
	if (!set || !name) return nullptr;
	scn::world_context* world = set->find(*name);
	return world ? &world->state() : nullptr;
}

std::optional<inp::input_target> edt::active_play_input_target(const editor_services& services, const scene_document& state)
{
	const auto contexts = services.ctx_manager.lock();
	const std::string* name = state.active_world_name();
	if (!contexts || !name || !contexts->find(k_play_set, *name)) return std::nullopt;
	return inp::input_target{ .set = std::string{ k_play_set }, .context = *name };
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
