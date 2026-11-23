#pragma once
#include "edt_document_tabs.h"
#include <functional>
#include <vector>

namespace gui { struct theme_semantic_colors; }

namespace edt
{
	// The strip of document tabs (#1184), drawn inside the dock host between the main menu and the dock space, so it is no
	// part of the dock tree and layout.ini stays valid. Only draws what edt_document_tabs.h gives it.
	class document_tabs_bar
	{
	public:
		struct actions
		{
			std::function<void(document_id)> activate;
			std::function<void(document_id)> close;
		};

		// Nothing is drawn (no ImGui call) without tabs.
		void draw(const std::vector<document_tab>& tabs, const gui::theme_semantic_colors& colors, const actions& on);

	private:
		// The tab ImGui showed selected last frame: a different one while the active document did not change is a click.
		document_id m_selected = 0;
	};
}
