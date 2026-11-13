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
		std::string parents;   // "res://a.desc, res://b.desc": what the file inherits from, nearest first; empty: nothing
		bool dirty = false;
		bool can_save = false;
		bool changed_on_disk = false;
		std::string chain_problem;
	};

	data_document_status status_of(const data_document& document);

	// The panel id and the ImGui window title of the window of document `id`: the label before "###" is shown, the rest
	// is the window's identity, so a renamed or retyped document keeps its place in the layout.
	std::string data_document_panel_id(document_id id);
	std::string data_document_window_title(const data_document& document, document_id id);
}
