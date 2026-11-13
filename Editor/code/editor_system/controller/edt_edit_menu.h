#pragma once
#include <snk/editor/edt_action_registry.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	class dialog_service;
	class document_manager;

	// Edit > Undo / Redo (#145) over the history of the active document. Nothing is undone while a dialog waits for
	// an answer (the change would land behind it) or while `gesture_active` (a drag or an edit in progress, an active
	// ImGui item); null: never. When no document is active or the active one has nothing to undo, Undo takes back what
	// `fallback_undo` does instead (the asset fixes of Problems), labelled "Undo <fallback_undo_label()>" and enabled
	// while that label is set.
	[[nodiscard]] std::vector<registration> register_edit_actions(
		action_registry& actions, document_manager& documents, const dialog_service& dialogs, std::function<bool()> gesture_active,
		std::function<std::optional<std::string>()> fallback_undo_label = {}, std::function<bool()> fallback_undo = {});
}
