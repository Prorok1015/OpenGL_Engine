#pragma once
#include "edt_shape_menu_model.h"
#include <snk/editor/edt_action_registry.h>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class prefab_document;

	// Why the open prefab's nodes are not changed by a rebuild: Play, a file of a lower layer, a project that may not be written. Empty: they follow.
	std::string prefab_rebuild_blocker(const prefab_document& prefab);

	// The outcome of a Rebuild asked from the Inspector: `error` when shape.rebuild refused (the files are as they were), `notes` when it
	// wrote the files but something around it did not follow (the level's nodes, the open prefab's working copy): a person is told each.
	struct rebuild_report
	{
		std::string error;
		std::vector<std::string> notes;
	};

	// shape.rebuild as a person's call, then the open prefab's working copy takes the new range of its nodes (`prefab` may be null).
	rebuild_report rebuild_shape(action_registry& actions, prefab_document* prefab, const boost::json::object& arguments, std::string_view geometry);

	// What a pick of the Add shape menu does in the prefab editor: a node with the shape's mesh goes under the picked node of the
	// working copy (an edit of the document, so Apply and Revert see it). A parametric shape's files are made by shape.create first.
	// The error text, empty when the node was added.
	std::string add_shape_to_prefab(action_registry& actions, prefab_document& prefab, const mesh::shape_request& request);
}
