#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include "edt_prefab_document.h"
#include "edt_scene_document.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

edt::document_manager::document_manager(scene_document& scene, editor_services& services, editor_events& events,
	res::resource_system& resources, desc::desc_system& descs)
	: m_scene(&scene)
	, m_services(services)
	, m_events(events)
	, m_resources(resources)
	, m_descs(descs)
{
	m_scene->assign_id(m_next_id++);
	m_scene->bind_events(&m_events);
	m_active = m_scene->id();
}

edt::document_manager::~document_manager() = default;

edt::scene_document& edt::document_manager::scene()
{
	ASSERT_MSG(m_scene, "the scene document is gone: document_manager::shutdown was called");
	return *m_scene;
}

const edt::scene_document& edt::document_manager::scene() const
{
	ASSERT_MSG(m_scene, "the scene document is gone: document_manager::shutdown was called");
	return *m_scene;
}

void edt::document_manager::shutdown()
{
	if (!m_scene) return;
	if (m_preview && m_preview->is_open()) close(m_preview->id());
	m_scene->bind_events(nullptr);
	m_scene = nullptr;
	m_active = 0;
}

edt::document* edt::document_manager::active()
{
	return find(m_active);
}

edt::document* edt::document_manager::find(document_id id)
{
	if (m_scene && id == m_scene->id()) return m_scene;
	if (m_preview && m_preview->is_open() && m_preview->id() == id) return m_preview.get();
	return nullptr;
}

void edt::document_manager::set_active(document_id id)
{
	if (!find(id) || id == m_active) return;
	m_active = id;
	m_events.document_activated.emit(id);
}

edt::prefab_document& edt::document_manager::open_prefab(const res::tag& prefab_tag)
{
	if (!m_preview) {
		m_preview = std::make_unique<prefab_document>(m_services, m_resources, m_descs);
	}
	if (m_preview->is_open()) {
		close(m_preview->id());
	}

	m_preview->open(prefab_tag);
	if (m_preview->is_open()) {
		m_preview->assign_id(m_next_id++);
		m_events.document_opened.emit(m_preview->id());
		set_active(m_preview->id());
	}
	return *m_preview;
}

edt::prefab_document* edt::document_manager::preview()
{
	return m_preview && m_preview->is_open() ? m_preview.get() : nullptr;
}

void edt::document_manager::close(document_id id)
{
	if (m_scene && id == m_scene->id()) {
		egLOG_WARN("Editor/Documents", "The scene document cannot be closed");
		return;
	}
	if (!m_preview || !m_preview->is_open() || m_preview->id() != id) return;

	m_preview->close();
	if (m_active == id) {
		m_active = m_scene ? m_scene->id() : 0;
		m_events.document_activated.emit(m_active);
	}
	m_events.document_closed.emit(id);
}

void edt::document_manager::for_each(const std::function<void(document&)>& fn)
{
	if (m_scene) fn(*m_scene);
	if (m_preview && m_preview->is_open()) fn(*m_preview);
}
