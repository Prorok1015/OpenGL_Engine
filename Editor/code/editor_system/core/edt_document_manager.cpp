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
	m_kind_removals = kinds.on_preview_removed([this](const asset_preview_desc& preview) { preview_removed(preview); });
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

edt::previewable_document& edt::document_manager::instance_of(const asset_preview_desc& preview)
{
	const auto known = std::find_if(m_previews.begin(), m_previews.end(), [&preview](const kept_preview& p) { return p.preview_id == preview.id; });
	if (known != m_previews.end()) return *known->instance;

	const auto make = preview.make_preview;
	auto made = make(*m_services);
	ASSERT_MSG(made, "make_preview of an asset preview made no document");
	m_previews.push_back({ preview.id, std::move(made) });
	return *m_previews.back().instance;
}

edt::previewable_document* edt::document_manager::open_with(const asset_preview_desc& preview, const res::tag& source)
{
	previewable_document& next = instance_of(preview);
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

edt::previewable_document* edt::document_manager::open_preview(const res::tag& source)
{
	ASSERT_MSG(m_kinds && m_services, "document_manager::set_preview_kinds was not called: there is no asset preview to make a document");
	const asset_preview_desc* preview = m_kinds->preview_for(source.relative());
	return preview ? open_with(*preview, source) : nullptr;
}

edt::previewable_document* edt::document_manager::open_preview(std::string_view preview_id, const res::tag& source)
{
	ASSERT_MSG(m_kinds && m_services, "document_manager::set_preview_kinds was not called: there is no asset preview to make a document");
	const asset_preview_desc* preview = m_kinds->find_preview(preview_id);
	return preview ? open_with(*preview, source) : nullptr;
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

void edt::document_manager::preview_removed(const asset_preview_desc& preview)
{
	const auto known = std::find_if(m_previews.begin(), m_previews.end(), [&preview](const kept_preview& p) { return p.preview_id == preview.id; });
	if (known == m_previews.end()) return;

	// Out of the list before anything is announced: a listener of the close may open another preview, which changes the list.
	std::unique_ptr<previewable_document> owned = std::move(known->instance);
	m_previews.erase(known);
	if (m_open_preview == owned.get()) {
		if (owned->is_dirty()) {
			// Nothing saves it any more: the code that made it is going away. Said where a person looks, not only in the log.
			const std::string message = "the preview '" + owned->title() + "' had unsaved changes and was closed: its asset preview '"
				+ preview.id + "' was unloaded";
			egLOG_WARN("Editor/Documents", "{}", message);
			if (m_problems) {
				problem item;
				item.severity = problem_severity::warning;
				item.file = std::string(owned->source().relative());
				item.message = message;
				m_problems->set(k_preview_problem_source, { std::move(item) });
			}
		}
		close(owned->id());
	}
	// `owned` goes here, after the announcements and while the preview's make_preview is still registered.
}

void edt::document_manager::for_each(const std::function<void(document&)>& fn)
{
	if (m_scene) fn(*m_scene);
	if (preview()) fn(*m_open_preview);
}
