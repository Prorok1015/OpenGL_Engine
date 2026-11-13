#include "edt_data_document_view.h"
#include "edt_data_document.h"
#include <format>

std::string edt::migration_note_text(int file_version, int current_version, bool project_migration)
{
	std::string text = std::format("Outdated file: v{} -> v{}. Opened migrated in memory; the file is not changed until you save, and saving writes v{}.",
		file_version, current_version, current_version);
	if (project_migration) {
		text += " Project > Migrate Project brings the whole project up to date.";
	}
	return text;
}

edt::data_document_status edt::status_of(const data_document& document, bool project_migration)
{
	data_document_status out;
	out.label = std::format("{} ({})", document.title(), document.type());
	out.path = std::string{ document.source().view() };
	for (const res::tag& parent : document.parents()) {
		if (!out.parents.empty()) out.parents += ", ";
		out.parents += parent.view();
	}
	out.dirty = document.is_dirty();
	out.migrated = document.migrated();
	if (out.migrated) {
		out.migration_note = migration_note_text(document.file_version(), document.current_version(), project_migration);
		for (const std::string& note : document.migration_notes()) {
			if (!out.migration_details.empty()) out.migration_details += '\n';
			out.migration_details += note;
		}
	}
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
