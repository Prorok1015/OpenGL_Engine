#pragma once
#include <snk/editor/edt_action_registry.h>
#include <functional>
#include <vector>

namespace edt
{
	class document_manager;

	// Edit > Undo / Redo (#145) over the history of the active document. `gesture_active`: a drag or an edit in
	// progress (an active ImGui item); undo waits for it to end. Null: never.
	[[nodiscard]] std::vector<registration> register_edit_actions(
		action_registry& actions, document_manager& documents, std::function<bool()> gesture_active);
}
