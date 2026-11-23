#pragma once
#include "edt_shape_menu_model.h"
#include <snk/editor/edt_action_registry.h>
#include <string>
#include <string_view>

namespace edt
{
	// What a pick of the Add shape menu does in a level: shape.add_base for a ready-made shape, shape.create then shape.add for a
	// parametric one, both as a person's calls. The error text, empty when the node was made. A parametric shape whose files were
	// made stays on disk when the node cannot be added (a parent inside a prefab instance): the person may add it by hand.
	std::string run_shape_request(action_registry& actions, std::string_view world, const mesh::shape_request& request);
}
