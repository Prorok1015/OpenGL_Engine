#pragma once
#include <snk/input/inp_input_router.h>
#include <optional>

namespace edt
{
	class scene_document;

	// The context the scene viewports feed with input: the play twin of the active world when the
	// set "play" has one, otherwise the active world of the set "level". Empty when there is none.
	std::optional<inp::input_target> active_input_target(const scene_document& state);

	// What the Game view feeds: only the play twin of the active world. Without a play context it
	// binds nothing, so the Game view sends no input in Edit mode (the editor camera stays put).
	std::optional<inp::input_target> active_play_input_target(const scene_document& state);
}
