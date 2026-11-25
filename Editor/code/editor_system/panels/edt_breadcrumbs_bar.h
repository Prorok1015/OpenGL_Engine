#pragma once
#include "edt_breadcrumbs.h"
#include <functional>
#include <vector>

namespace gui { struct theme_semantic_colors; }

namespace edt
{
	// The crumbs of one window (EPIC-82 6, #1402): a button each, the last one text, `*` and the `warning` colour for unsaved changes. A crumb can
	// be dragged: onto the crumb bar of another window (dock-back) or onto empty space (tear-off); the drag-and-drop is ours, ImGui's docking
	// cannot take a tab out of a bar of our own. Draws the rows breadcrumbs_of gave and reports what the person did.
	class breadcrumbs_bar
	{
	public:
		struct actions
		{
			// A crumb that is not the current one was clicked.
			std::function<void(document_id)> navigate;
			// A crumb was let go: `drop` says where.
			std::function<void(document_id link, const crumb_drop& drop)> dropped;
		};

		// Nothing is drawn (no ImGui call) without rows. `host`: this window's host, named by a drop on its bar. `allow_move` false: the crumbs are
		// not drag sources (a window that cannot yet show another link in place).
		void draw(const std::vector<breadcrumb>& rows, host_id host, bool allow_move, const gui::theme_semantic_colors& colors, const actions& on);

	private:
		document_id m_dragged = 0;
		bool m_accepted = false;
	};
}
