#pragma once
#include <snk/editor/edt_document.h>
#include <string>

namespace edt
{
	class data_document;

	// What the window of a data document shows besides its fields (no ImGui, unit-tested).
	struct data_document_status
	{
		std::string label;     // "game (test.config)": the file's name and its type
		std::string path;      // res://config/game.desc
		std::string parents;   // the paths of what the file inherits from, nearest first, comma separated; empty: nothing
		bool dirty = false;
		// The file is of an older version and was migrated in memory (S56-6): saving writes the new one, after a question.
		bool migrated = false;
		std::string migration_note; // the badge: empty unless migrated
		std::string migration_details; // what the steps warned about, one per line; empty: nothing
		bool can_save = false;
		bool changed_on_disk = false;
		std::string chain_problem;
	};

	// "Outdated file: v1 -> v2. Opened migrated in memory; the file is not changed until you save, and saving writes v2." With
	// `project_migration` (the editor has a project migration to point to) the badge names it too.
	// `nested_outdated`: the root is current and only objects typed inside the file are older.
	std::string migration_note_text(int file_version, int current_version, bool nested_outdated, bool project_migration);

	// `project_migration`: Project > Migrate Project exists (UI of #761); until then the badge does not point to it.
	data_document_status status_of(const data_document& document, bool project_migration = false);

	// The panel id and the ImGui window title of the window of document `id`: the label before "###" is shown, the rest
	// is the window's identity, so a renamed or retyped document keeps its place in the layout.
	std::string data_document_panel_id(document_id id);
	std::string data_document_window_title(const data_document& document, document_id id);
}
