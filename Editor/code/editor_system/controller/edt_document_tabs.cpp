#include "edt_document_tabs.h"
#include <snk/editor/edt_document_manager.h>
#include <utility>

std::vector<edt::document_tab> edt::tabs_of(document_manager& documents)
{
	std::vector<document_tab> out;
	const document_id scene_id = documents.scene().id();
	const document_id active_id = documents.active()->id();
	documents.for_each([&](document& open) {
		out.push_back({ open.id(), std::string(open.kind()), open.title(), open.is_dirty(), open.id() == active_id, open.id() != scene_id });
	});
	return out;
}

void edt::focus_document(document_manager& documents, document_id id, const document_focus_ports& ports)
{
	const document* open = documents.find(id);
	if (!open) return;
	if (open->kind() == "data") {
		if (ports.show_data) ports.show_data(open->source());
		return;
	}
	if (open->kind() == "prefab" && ports.show_prefab) ports.show_prefab();
	documents.set_active(id);
}

edt::document_tabs::document_tabs(document_manager& documents, document_tab_ports ports)
	: m_documents(documents)
	, m_ports(std::move(ports))
{
}

std::vector<edt::document_tab> edt::document_tabs::tabs() const
{
	return tabs_of(m_documents);
}

void edt::document_tabs::activate(document_id id)
{
	focus_document(m_documents, id, m_ports.focus);
}

bool edt::document_tabs::close(document_id id)
{
	const document* open = m_documents.find(id);
	if (!open || id == m_documents.scene().id()) return false;
	if (open->kind() == "data" && m_ports.request_close_data) {
		m_ports.request_close_data(id);
		return true;
	}
	if (open->kind() == "prefab" && m_ports.request_close_prefab) {
		m_ports.request_close_prefab();
		return true;
	}
	return false;
}
