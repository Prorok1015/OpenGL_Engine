#pragma once
#include <snk/editor/edt_action_registry.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	class dialog_service;
	class document;
	class document_manager;

	// Edit > Undo / Redo (#145) over the history of the active document. Nothing is undone while a dialog waits for
	// an answer (the change would land behind it) or while `gesture_active` (a drag or an edit in progress, an active
	// ImGui item); null: never. With both `asset_undo_label` and `asset_undo` set, also "edit.undo_asset_action"
	// (Edit > Undo Asset Action, no shortcut, not for agents): takes back the last asset fix of Problems, labelled
	// "Undo Asset Action: <asset_undo_label()>" and enabled while that label is set. Edit > Undo never touches them.
	// `history_owner` names the document Undo and Redo act on instead of the active one (the Inspector is on a prefab node: its edits
	// are the prefab's, the level's history must not answer for them); null from it, or no callback: the active document.
	[[nodiscard]] std::vector<registration> register_edit_actions(
		action_registry& actions, document_manager& documents, const dialog_service& dialogs, std::function<bool()> gesture_active,
		std::function<std::optional<std::string>()> asset_undo_label = {}, std::function<bool()> asset_undo = {},
		std::function<document*()> history_owner = {});
}
