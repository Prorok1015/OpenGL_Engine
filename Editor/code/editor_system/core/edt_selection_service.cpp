#include <snk/editor/edt_selection_service.h>
#include <snk/scene/level/scn_node_index.h>
#include <entt/entt.hpp>

edt::selection_service::selection_service(editor_events& events)
	: m_events(events)
{
	// Subscribed first, so the selection is already empty for whoever listens to the same event afterwards.
	m_world_changed = m_events.active_world_changed.subscribe([this](document_id) { clear(); });
}

void edt::selection_service::select(selection value)
{
	m_current = std::move(value);
	emit_current();
}

void edt::selection_service::clear()
{
	m_current = {};
	emit_current();
}

void edt::selection_service::emit_current()
{
	// A slot may select or clear again; slots after it must still read the value this emit announced.
	const selection announced = m_current;
	m_events.selection_changed.emit(announced);
}

ecs::entity edt::selection_service::entity() const
{
	if (m_current.empty() || !m_registry_of)
		return entt::null;
	entt::registry* reg = m_registry_of(m_current.world);
	if (!reg)
		return entt::null;
	return scn::node_index::get(*reg).find(m_current.path);
}
