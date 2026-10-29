#include "edt_scene_document.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/common/logger/engine_log.h>
#include <exception>

void edt::scene_document::commit(std::string_view label, commit_mode mode)
{
	egLOG_TRACE("Editor/Commit", "{}", label);
	touch_descs();
	if (mode == commit_mode::state_only) {
		// The world list changed: the edits before cannot be undone into it.
		m_unrecorded = true;
		reset_history();
	} else {
		record_edit(label);
	}
	// state_only edits (a new level, a new world) were never announced; only a push that did something is.
	if (mode == commit_mode::push && m_pusher && m_pusher()) {
		notify_changed();
	}
}

void edt::scene_document::record_edit(std::string_view label)
{
	std::unique_ptr<history_change> change;
	switch (m_index.diff(world_descs, change, m_last_stats)) {
	case scene_history_index::diff_result::changed:
		m_history.record(std::string{ label }, m_origin, std::move(change));
		break;
	case scene_history_index::diff_result::unchanged:
		break;
	case scene_history_index::diff_result::unavailable:
		if (m_index.valid()) {
			egLOG_WARN("Editor/History", "'{}' is not recorded: the worlds cannot be compared with the last commit", label);
		}
		m_unrecorded = true;
		m_history.clear();
		m_history.mark_saved();
		m_index.rebuild(world_descs);
		break;
	}
}

void edt::scene_document::mark_clean()
{
	m_unrecorded = false;
	m_history.mark_saved();
}

bool edt::scene_document::undo()
{
	if (!m_history.can_undo()) {
		return false;
	}
	apply_history("Undo", [this] { m_history.undo(); });
	return true;
}

bool edt::scene_document::redo()
{
	if (!m_history.can_redo()) {
		return false;
	}
	apply_history("Redo", [this] { m_history.redo(); });
	return true;
}

edt::rollback_result edt::scene_document::rollback_group(std::uint64_t group)
{
	rollback_result result;
	// Rolls nothing back (no group, a conflict at its newest edit): nobody is told the worlds change.
	auto before = m_before_apply;
	m_before_apply = nullptr;
	const bool applied = apply_history("Rollback", [&] {
		m_history.rollback_group(group, result, [&] {
			if (before) {
				before();
			}
		});
		// Undone out of order: the nodes around the undone ones were not part of the changes, so the index takes the
		// worlds as they are now rather than as the changes left them.
		if (result.rolled_back > 0) {
			m_index.rebuild(world_descs);
		}
	});
	m_before_apply = std::move(before);
	result.failed = !applied;
	return result;
}

bool edt::scene_document::apply_history(std::string_view what, const std::function<void()>& apply)
{
	bool ok = true;
	try {
		if (m_before_apply) {
			m_before_apply();
		}
		apply();
	} catch (const std::exception& e) {
		recover_from_apply(what, e.what());
		ok = false;
	} catch (...) {
		recover_from_apply(what, "unknown exception");
		ok = false;
	}
	try {
		finish_apply();
	} catch (const std::exception& e) {
		recover_from_apply(what, e.what());
		ok = false;
	} catch (...) {
		recover_from_apply(what, "unknown exception");
		ok = false;
	}
	return ok;
}

void edt::scene_document::recover_from_apply(std::string_view what, std::string_view error)
{
	// Half applied: what the worlds hold now is neither state of the history. Keep it, reload it, start over.
	egLOG_ERROR("Editor/History", "{} failed ({}): the history is cleared and the worlds are reloaded as they are", what, error);
	m_history.clear();
	m_unrecorded = true;
	m_history.mark_saved();
	m_index.rebuild(world_descs);
	for (std::size_t world = 0; world < world_descs.size(); ++world) {
		m_index.touch(world);
	}
}

void edt::scene_document::reset_history()
{
	const bool dirty = is_dirty();
	m_history.clear();
	m_unrecorded = dirty;
	m_history.mark_saved();
	if (!m_index.rebuild(world_descs)) {
		egLOG_WARN("Editor/History", "The level has a node without an id or an id twice: its edits are not recorded");
	}
}

void edt::scene_document::rebase_history()
{
	m_index.rebuild(world_descs);
}

void edt::scene_document::finish_apply()
{
	touch_descs();
	// An undo can change a world that is not the active one: its context is rebuilt as well, or it would keep the
	// state before (and a save would write its live transforms back over the undone desc).
	bool pushed = false;
	for (const std::size_t world : m_index.take_touched()) {
		if (static_cast<int>(world) == active_world_idx) {
			pushed = (m_pusher && m_pusher()) || pushed;
		} else if (m_world_reloader) {
			pushed = m_world_reloader(world) || pushed;
		}
	}
	if (pushed) {
		notify_changed();
	}
}

void edt::scene_document::notify_changed()
{
	if (m_events) {
		m_events->document_changed.emit(id(), desc_generation);
	}
}

edt::save_result edt::scene_document::save()
{
	if (!can_save()) return save_result::failure("no level is loaded");
	return m_save() ? save_result::ok() : save_result::failure("the level could not be written");
}

edt::save_result edt::scene_document::save_as(const res::tag& target)
{
	if (!can_save() || !m_save_as) return save_result::failure("no level is loaded");
	return m_save_as(target) ? save_result::ok() : save_result::failure("the level could not be written");
}
