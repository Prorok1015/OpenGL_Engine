#pragma once
#include "edt_save_all.h"
#include <functional>
#include <memory>

namespace edt
{
	class dialog_service;
	class prefab_document;

	// What the question of a prefab Save All could not apply needs to run (#1183): the dialogs, and the document as it is when the
	// answer comes (null: it is gone meanwhile; the prefab editor looks it up by id).
	struct prefab_question_ports
	{
		dialog_service& dialogs;
		std::function<prefab_document*()> open;
		// Expires with the owner of the dialog: an answer after that does nothing (`done` is not called).
		std::weak_ptr<void> alive;
	};

	// Asks the question `question` of `prefab`, a prefab Apply refused: a file that changed on disk is Reload / Overwrite / Cancel, a
	// variant whose edits need its base dropped is Save without base / Cancel. `done(true)` once the prefab was reloaded or written,
	// `done(false)` after Cancel, a failure (said in a message) or for a question a prefab has none of.
	void ask_prefab_question(const prefab_question_ports& ports, const prefab_document& prefab, save_all_question question,
		std::function<void(bool resolved)> done);
}
