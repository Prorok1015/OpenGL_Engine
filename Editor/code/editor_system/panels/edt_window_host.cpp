#include "edt_window_host.h"

edt::window_host::window_host(document_manager& documents, document_id link, ports on)
	: m_documents(documents), m_link(link), m_ports(std::move(on))
{
}

edt::host_id edt::window_host::host()
{
	if (const std::optional<host_id> known = m_documents.host_of(m_link)) return *known;
	return m_documents.open_host(m_link);
}

void edt::window_host::draw(const gui::theme_semantic_colors& colors)
{
	const host_id own = host();
	const std::vector<breadcrumb> rows = breadcrumbs_of(m_documents, m_link);
	breadcrumbs_bar::actions on;
	on.navigate = [this](document_id id) {
		if (m_ports.show_link) m_ports.show_link(id);
	};
	on.dropped = [this](document_id link, const crumb_drop& drop) {
		if (apply_crumb_drop(m_documents, link, drop) != 0 && m_ports.show_link) m_ports.show_link(link);
	};
	m_bar.draw(rows, own, m_ports.allow_move, colors, on);
}
