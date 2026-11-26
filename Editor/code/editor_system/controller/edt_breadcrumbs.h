#pragma once
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_manager.h>
#include <string>
#include <vector>

namespace edt
{
	// The breadcrumbs of a window (EPIC-82 6, #1402), no ImGui: `Level > Card#3 > Material "Face"`, or `Asset > Material "Face"`
	// for a window opened from the asset browser. panels/edt_breadcrumbs_bar.h draws these rows.
	struct breadcrumb
	{
		// The document the crumb stands for; 0 for the label that names where an asset window was opened from.
		document_id id = 0;
		std::string label;
		bool dirty = false;
		// The last crumb: the link the window shows. Not a button.
		bool current = false;
		// A crumb a person can drag out of the window or onto another: any link (an asset window too), not the root document of a chain (the level) and not the label.
		bool draggable = false;
	};

	inline constexpr const char* k_asset_origin_label = "Asset";

	// The crumbs of the chain `link` ends. Two open links with one title under one parent say their anchor (`Card (node:3)`).
	std::vector<breadcrumb> breadcrumbs_of(document_manager& documents, document_id link);

	// Where a dragged crumb was let go.
	enum class drop_kind { none, empty_space, breadcrumb_bar };

	struct crumb_drop
	{
		drop_kind kind = drop_kind::none;
		// breadcrumb_bar: the host whose bar it was.
		host_id host = 0;
	};

	// What letting go of `link` means: tear-off on empty space, dock-back on another host's bar, nothing on its own host's bar (or
	// on a link that is not one). The id of the host that shows `link` afterwards, 0 when nothing was done.
	host_id apply_crumb_drop(document_manager& documents, document_id link, const crumb_drop& drop);
}
