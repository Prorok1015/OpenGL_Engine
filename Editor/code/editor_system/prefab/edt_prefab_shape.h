#pragma once
#include "edt_shape_menu_model.h"
#include <snk/editor/edt_action_registry.h>
#include <string>

namespace edt
{
	class prefab_document;

	// What a pick of the Add shape menu does in the prefab editor: a node with the shape's mesh goes under the picked node of the
	// working copy (an edit of the document, so Apply and Revert see it). A parametric shape's files are made by shape.create first.
	// The error text, empty when the node was added.
	std::string add_shape_to_prefab(action_registry& actions, prefab_document& prefab, const mesh::shape_request& request);
}
