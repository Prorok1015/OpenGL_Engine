#pragma once
#include "edt_prefab_document.h"
#include <snk/editor/edt_dialog_service.h>
#include <functional>
#include <string>

namespace edt
{
	// What the prefab editor's buttons show: no ImGui, so a test can look at it.
	struct prefab_toolbar_state
	{
		bool apply_enabled = false;
		// Why Apply is off, empty when it is on or when nothing is edited.
		std::string apply_hint;
		bool revert_enabled = false;
	};
	prefab_toolbar_state toolbar_state_of(const prefab_document& prefab);

	// The ports of closing a prefab document that may have unapplied edits (the editor wires the dialog service in).
	struct prefab_close_ports
	{
		// Asks Save / Discard / Cancel and calls the answer back.
		std::function<void(std::function<void(unsaved_choice)>)> ask;
		// Applies the document.
		std::function<save_result()> apply;
		// Shown when Apply failed (the document stays open).
		std::function<void(const std::string& error)> report_failure;
	};

	// A clean document closes (proceed) at once. A dirty one asks: Cancel and a failed Save leave it open, Save (applied) and
	// Discard proceed.
	void close_prefab_with_question(bool dirty, const prefab_close_ports& ports, std::function<void()> proceed);
}
