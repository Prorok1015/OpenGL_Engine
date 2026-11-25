#include "edt_breadcrumbs.h"
#include <algorithm>
#include <format>

namespace edt
{
	std::vector<breadcrumb> breadcrumbs_of(document_manager& documents, document_id link)
	{
		std::vector<breadcrumb> rows;
		const std::vector<document_id> chain = documents.chain_of(link);
		if (chain.empty()) return rows;

		if (documents.has_link(chain.front()) && documents.link_kind_of(chain.front()) == link_kind::asset) {
			rows.push_back({ 0, k_asset_origin_label, false, false, false });
		}
		for (const document_id id : chain) {
			const document* open = documents.find(id);
			if (!open) continue;
			breadcrumb row;
			row.id = id;
			row.label = open->title();
			row.dirty = open->is_dirty();
			row.draggable = documents.has_link(id);
			if (documents.has_link(id) && documents.link_kind_of(id) == link_kind::instance) {
				const document_id parent = documents.link_parent(id);
				const auto same_title = [&](document_id other) {
					const document* other_doc = documents.find(other);
					return other != id && documents.link_kind_of(other) == link_kind::instance && documents.link_parent(other) == parent
						&& other_doc && other_doc->title() == row.label;
				};
				const std::vector<document_id> siblings = documents.descendants_of(parent);
				if (std::ranges::any_of(siblings, same_title)) row.label = std::format("{} ({})", row.label, documents.link_anchor(id));
			}
			rows.push_back(std::move(row));
		}
		if (!rows.empty()) rows.back().current = true;
		return rows;
	}

	host_id apply_crumb_drop(document_manager& documents, document_id link, const crumb_drop& drop)
	{
		if (!documents.has_link(link)) return 0;
		switch (drop.kind) {
		case drop_kind::empty_space:
			return documents.move_link(link, 0);
		case drop_kind::breadcrumb_bar: {
			const std::optional<host_id> own = documents.host_of(link);
			if (drop.host == 0 || (own && *own == drop.host)) return 0;
			return documents.move_link(link, drop.host);
		}
		case drop_kind::none:
			break;
		}
		return 0;
	}
}
