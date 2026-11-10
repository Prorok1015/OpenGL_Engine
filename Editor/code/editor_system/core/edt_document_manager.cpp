#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>

namespace {
constexpr const char* k_preview_problem_source = "preview";
}

edt::document_manager::document_manager(document& scene, editor_events& events)
	: m_scene(&scene)
	, m_events(events)
{
	m_scene->assign_id(m_next_id++);
	m_scene->bind_events(&m_events);
	m_active = m_scene->id();
}

edt::document_manager::~document_manager() = default;

void edt::document_manager::set_preview_kinds(asset_kind_registry& kinds, editor_services& services, problem_list* problems)
{
	m_kinds = &kinds;
	m_services = &services;
	m_problems = problems;
	m_kind_removals = kinds.on_removed([this](const asset_kind_desc& kind) { kind_removed(kind); });
}

edt::document& edt::document_manager::scene()
{
	ASSERT_MSG(m_scene, "the scene document is gone: document_manager::shutdown was called");
	return *m_scene;
}

const edt::document& edt::document_manager::scene() const
{
	ASSERT_MSG(m_scene, "the scene document is gone: document_manager::shutdown was called");
	return *m_scene;
}

void edt::document_manager::shutdown()
{
	if (!m_scene) return;
	if (m_open_preview) close(m_open_preview->id());
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
	if (m_open_preview && m_open_preview->is_open() && m_open_preview->id() == id) return m_open_preview;
	return nullptr;
}

void edt::document_manager::set_active(document_id id)
{
	if (!find(id) || id == m_active) return;
	m_active = id;
	m_events.document_activated.emit(id);
}

edt::previewable_document& edt::document_manager::preview_for(const asset_kind_desc& kind)
{
	const auto known = std::find_if(m_previews.begin(), m_previews.end(), [&kind](const kind_preview& p) { return p.kind_id == kind.id; });
	if (known != m_previews.end()) return *known->instance;

	const auto make = kind.make_preview;
	auto made = make(*m_services);
	ASSERT_MSG(made, "make_preview of an asset kind made no document");
	m_previews.push_back({ kind.id, std::move(made) });
	return *m_previews.back().instance;
}

edt::previewable_document* edt::document_manager::open_preview(const res::tag& source)
{
	ASSERT_MSG(m_kinds && m_services, "document_manager::set_preview_kinds was not called: there is no asset kind to make a preview");
	const asset_kind_desc* kind = m_kinds->kind_for_preview(source.relative());
	if (!kind) return nullptr;

	previewable_document& next = preview_for(*kind);
	if (m_open_preview) close(m_open_preview->id());
	if (m_problems) m_problems->clear(k_preview_problem_source);

	next.open(source);
	if (next.is_open()) {
		m_open_preview = &next;
		next.assign_id(m_next_id++);
		m_events.document_opened.emit(next.id());
		set_active(next.id());
	}
	return &next;
}

edt::previewable_document* edt::document_manager::preview()
{
	return m_open_preview && m_open_preview->is_open() ? m_open_preview : nullptr;
}

void edt::document_manager::close(document_id id)
{
	if (m_scene && id == m_scene->id()) {
		egLOG_WARN("Editor/Documents", "The scene document cannot be closed");
		return;
	}
	if (!m_open_preview || !m_open_preview->is_open() || m_open_preview->id() != id) return;

	m_open_preview->close();
	m_open_preview = nullptr;
	if (m_active == id) {
		m_active = m_scene ? m_scene->id() : 0;
		m_events.document_activated.emit(m_active);
	}
	m_events.document_closed.emit(id);
}

void edt::document_manager::kind_removed(const asset_kind_desc& kind)
{
	if (!kind.make_preview) return;
	const auto known = std::find_if(m_previews.begin(), m_previews.end(), [&kind](const kind_preview& p) { return p.kind_id == kind.id; });
	if (known == m_previews.end()) return;

	previewable_document& doc = *known->instance;
	if (m_open_preview == &doc) {
		if (doc.is_dirty()) {
			// Nothing saves it any more: the kind's code is going away. Said where a person looks, not only in the log.
			const std::string message = "the preview '" + doc.title() + "' had unsaved changes and was closed: its asset kind '"
				+ kind.id + "' was unloaded";
			egLOG_WARN("Editor/Documents", "{}", message);
			if (m_problems) {
				problem item;
				item.severity = problem_severity::warning;
				item.file = std::string(doc.source().relative());
				item.message = message;
				m_problems->set(k_preview_problem_source, { std::move(item) });
			}
		}
		close(doc.id());
	}
	// The document may hold what the kind's code made: it goes before the kind does.
	m_previews.erase(known);
}

void edt::document_manager::for_each(const std::function<void(document&)>& fn)
{
	if (m_scene) fn(*m_scene);
	if (preview()) fn(*m_open_preview);
}
