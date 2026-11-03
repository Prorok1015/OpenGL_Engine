#pragma once
#include <snk/input/inp_input_router.h>
#include <entt/fwd.hpp>
#include <optional>
#include <string_view>

namespace scn { class context_set; }

namespace edt
{
	class scene_document;
	struct editor_services;

	// The set play_session builds from the document while Play runs.
	inline constexpr std::string_view k_play_set = "play";

	// The set the scene views (Scene viewport, Game view, viewport.capture, the selected entity) show: "play" while
	// it exists, otherwise "level". Null without a context manager.
	scn::context_set* view_set(const editor_services& services);
	// The active world's registry in view_set(); null when there is no such world.
	entt::registry* view_registry(const editor_services& services, const scene_document& state);

	// The context the scene viewports feed with input: the play twin of the active world when the
	// set "play" has one, otherwise the active world of the set "level". Empty when there is none.
	std::optional<inp::input_target> active_input_target(const editor_services& services, const scene_document& state);

	// What the Game view feeds: only the play twin of the active world. Without a play context it
	// binds nothing, so the Game view sends no input in Edit mode (the editor camera stays put).
	std::optional<inp::input_target> active_play_input_target(const editor_services& services, const scene_document& state);
}
