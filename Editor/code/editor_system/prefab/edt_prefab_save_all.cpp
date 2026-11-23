#include "edt_prefab_save_all.h"
#include "edt_prefab_document.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_dialog_service.h>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace {
	void tell_failure(edt::dialog_service& dialogs, const edt::prefab_document& open, std::string_view what, const std::string& error)
	{
		egLOG_WARN("editor", "{}", error);
		dialogs.message(edt::message_level::error, std::format("'{}' was not {}:\n{}", open.title(), what, error));
	}
}

void edt::ask_prefab_question(const prefab_question_ports& ports, const prefab_document& prefab, save_all_question question,
	std::function<void(bool resolved)> done)
{
	if (!done) return;
	if (question == save_all_question::disk_changed) {
		ports.dialogs.choose("File changed on disk",
			std::format("'{}' was changed on disk since you opened it.\n\nReload reads the file again and drops your unapplied edits. "
				"Overwrite replaces the file with your version.", prefab.title()),
			{ "Reload (drop my edits)", "Overwrite the file", "Cancel" },
			[ports, done = std::move(done)](std::optional<std::size_t> choice) {
				if (ports.alive.expired()) return;
				prefab_document* open = ports.open ? ports.open() : nullptr;
				if (!open || !choice || *choice > 1) {
					done(false);
					return;
				}
				if (*choice == 0) {
					const std::string error = open->reload();
					if (!error.empty()) tell_failure(ports.dialogs, *open, "reloaded", error);
					done(error.empty());
					return;
				}
				const save_result written = open->overwrite();
				if (!written.saved) tell_failure(ports.dialogs, *open, "written", written.error);
				done(written.saved);
			});
		return;
	}
	if (question == save_all_question::base_layer) {
		ports.dialogs.choose("Apply to a variant",
			std::format("'{}' is a variant whose edits no layer over its base can say.\n\nThe edits stay in the window. The file can be saved "
				"whole instead, without '__parent': it then stops following its base prefab.", prefab.title()),
			{ "Save without base", "Cancel" },
			[ports, done = std::move(done)](std::optional<std::size_t> choice) {
				if (ports.alive.expired()) return;
				prefab_document* open = ports.open ? ports.open() : nullptr;
				if (!open || choice != std::optional<std::size_t>{ 0 }) {
					done(false);
					return;
				}
				const save_result whole = open->save_without_base();
				if (!whole.saved) tell_failure(ports.dialogs, *open, "saved", whole.error);
				done(whole.saved);
			});
		return;
	}
	done(false);
}
