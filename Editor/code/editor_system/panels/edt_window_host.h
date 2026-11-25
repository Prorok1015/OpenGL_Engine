#pragma once
#include "edt_breadcrumbs_bar.h"
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_manager.h>
#include <functional>

namespace gui { struct theme_semantic_colors; }

namespace edt
{
	// The window-host part of a window (EPIC-82 5.4, 11.3, #1402): keeps the window's host in the document manager's table, draws the crumbs of the
	// link the window shows and turns what the person did with them into the model's calls. It draws nothing else: the window draws its panels.
	class window_host
	{
	public:
		struct ports
		{
			// Brings the window of `link` to the front, opening it when it has none (a click on a crumb; the window a drop made a host for).
			std::function<void(document_id link)> show_link;
			// The crumbs are drag sources: false while the window cannot show another link in place.
			bool allow_move = false;
		};

		window_host(document_manager& documents, document_id link, ports on);

		// The window's host, made when the table has none for the link (a host closed behind the window's back, a torn-off link).
		host_id host();
		void draw(const gui::theme_semantic_colors& colors);

	private:
		document_manager& m_documents;
		document_id m_link;
		ports m_ports;
		breadcrumbs_bar m_bar;
	};
}
