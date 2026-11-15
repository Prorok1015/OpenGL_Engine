#include "edt_prefab_close.h"

edt::prefab_toolbar_state edt::toolbar_state_of(const prefab_document& prefab)
{
	prefab_toolbar_state state;
	const std::string blocker = prefab.apply_blocker();
	state.apply_enabled = blocker.empty() && prefab.is_dirty() && !prefab.changed_on_disk();
	if (!blocker.empty()) {
		state.apply_hint = blocker;
	} else if (prefab.changed_on_disk()) {
		state.apply_hint = "The file changed on disk: Reload or Overwrite first";
	}
	state.revert_enabled = prefab.is_open() && !prefab.edits_locked();
	return state;
}

void edt::close_prefab_with_question(bool dirty, const prefab_close_ports& ports, std::function<void()> proceed)
{
	if (!dirty) {
		proceed();
		return;
	}
	ports.ask([ports, proceed = std::move(proceed)](unsaved_choice choice) {
		if (choice == unsaved_choice::cancel) return;
		if (choice == unsaved_choice::save) {
			const save_result saved = ports.apply();
			if (!saved.saved) {
				if (ports.report_failure) ports.report_failure(saved.error);
				return;
			}
		}
		proceed();
	});
}
