#pragma once
#include <snk/editor/edt_action_registry.h>
#include <functional>
#include <vector>

namespace edt
{
	class dialog_service;
	class document_manager;

	// Edit > Undo / Redo (#145) over the history of the active document. Nothing is undone while a dialog waits for
	// an answer (the change would land behind it) or while `gesture_active` (a drag or an edit in progress, an active
	// ImGui item); null: never.
	[[nodiscard]] std::vector<registration> register_edit_actions(
		action_registry& actions, document_manager& documents, const dialog_service& dialogs, std::function<bool()> gesture_active);
}
