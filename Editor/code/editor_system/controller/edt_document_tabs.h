#pragma once
#include <snk/editor/edt_document.h>
#include <snk/model_codec/res_tag.h>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	class document_manager;

	// The strip of tabs under the main menu (#1184, EPIC-59): one tab per open document. No ImGui in here: the strip
	// (panels/edt_document_tabs_bar.h) draws these rows and calls activate/close.

	struct document_tab
	{
		document_id id = 0;
		std::string kind;
		std::string title;
		bool dirty = false;
		bool active = false;
		// Only a data document and the prefab have a close flow of their own; the scene is always there, and a close is not
		// offered while it is blocked (document_tab_ports::close_blocked).
		bool closable = false;
	};

	// The open documents: the scene, then the prefab, then the data documents in the order they were opened (the order of
	// list_unsaved).
	std::vector<document_tab> tabs_of(document_manager& documents);

	// What bringing a document to the front takes that the document manager does not know.
	struct document_focus_ports
	{
		// The floating window of a data document (opening what is open moves the focus to its window).
		std::function<void(const res::tag& source)> show_data;
		// The prefab editor's window.
		std::function<void()> show_prefab;
	};

	// Makes `id` the active document and brings its window to the front. An id that names no open document is ignored. The one
	// way a tab and a row of the Unsaved list focus a document.
	void focus_document(document_manager& documents, document_id id, const document_focus_ports& ports);

	struct document_tab_ports
	{
		document_focus_ports focus;
		// The window's own close flow: closes at once when clean, else asks Save / Discard / Cancel. Nothing is lost on Cancel.
		std::function<void(document_id id)> request_close_data;
		std::function<void(document_id id)> request_close_prefab;
		// True while a close must not start (a Save All asks its questions: the document it still asks about must not go).
		std::function<bool()> close_blocked;
	};

	class document_tabs
	{
	public:
		document_tabs(document_manager& documents, document_tab_ports ports);

		std::vector<document_tab> tabs() const;
		// A click on a tab.
		void activate(document_id id);
		// The tab's close button: through the document's existing close question. False for the scene and for an id that
		// names no open document (nothing was asked).
		bool close(document_id id);

	private:
		bool closing_blocked() const;

		document_manager& m_documents;
		document_tab_ports m_ports;
	};
}
