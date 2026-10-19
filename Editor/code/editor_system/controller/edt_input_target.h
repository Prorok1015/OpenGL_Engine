#pragma once
#include <snk/input/inp_input_router.h>
#include <optional>

namespace edt
{
	struct shared_state;

	// The context the scene viewports feed with input: the play twin of the active world when the
	// set "play" has one, otherwise the active world of the set "level". Empty when there is none.
	std::optional<inp::input_target> active_input_target(const shared_state& state);
}
