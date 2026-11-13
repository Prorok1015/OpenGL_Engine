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
	// ImGui item); null: never. With both `asset_undo_label` and `asset_undo` set, also "edit.undo_asset_action"
	// (Edit > Undo Asset Action, no shortcut, not for agents): takes back the last asset fix of Problems, labelled
	// "Undo Asset Action: <asset_undo_label()>" and enabled while that label is set. Edit > Undo never touches them.
	[[nodiscard]] std::vector<registration> register_edit_actions(
		action_registry& actions, document_manager& documents, const dialog_service& dialogs, std::function<bool()> gesture_active,
		std::function<std::optional<std::string>()> asset_undo_label = {}, std::function<bool()> asset_undo = {});
}
