#include "edt_scene_document.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/common/logger/engine_log.h>

scn::context_set* edt::scene_document::level_set() const
{
	const auto contexts = ctx_manager.lock();
	return contexts ? &contexts->level_set() : nullptr;
}

void edt::scene_document::commit(std::string_view label, commit_mode mode)
{
	egLOG_TRACE("Editor/Commit", "{}", label);
	m_dirty = true;
	touch_descs();
	// state_only edits (a new level, a new world) were never announced; only a push that did something is.
	if (mode == commit_mode::push && m_pusher && m_pusher()) {
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
