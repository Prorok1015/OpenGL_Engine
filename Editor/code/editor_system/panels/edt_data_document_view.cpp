#include "edt_data_document_view.h"
#include "edt_data_document.h"
#include <format>

edt::data_document_status edt::status_of(const data_document& document)
{
	data_document_status out;
	out.label = std::format("{} ({})", document.title(), document.type());
	out.path = std::string{ document.source().view() };
	for (const res::tag& parent : document.parents()) {
		if (!out.parents.empty()) out.parents += ", ";
		out.parents += parent.view();
	}
	out.dirty = document.is_dirty();
	out.can_save = document.can_save();
	out.changed_on_disk = document.changed_on_disk();
	out.chain_problem = document.chain_problem();
	return out;
}

std::string edt::data_document_panel_id(document_id id)
{
	return std::format("data_doc:{}", id);
}

std::string edt::data_document_window_title(const data_document& document, document_id id)
{
	return std::format("{}###{}", status_of(document).label, data_document_panel_id(id));
}
