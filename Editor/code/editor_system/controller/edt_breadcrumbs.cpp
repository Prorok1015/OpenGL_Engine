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

		const std::optional<open_context> own = documents.link_of(chain.front());
		if (own && own->kind == link_kind::asset) {
			rows.push_back({ 0, k_asset_origin_label, false, false, false });
		}
		for (const document_id id : chain) {
			const document* open = documents.find(id);
			if (!open) continue;
			breadcrumb row;
			row.id = id;
			row.label = open->title();
			row.dirty = open->is_dirty();
			row.draggable = documents.link_of(id).has_value();
			if (const std::optional<open_context> context = documents.link_of(id); context && context->kind == link_kind::instance) {
				const auto same_title = [&](document_id other) {
					const std::optional<open_context> other_context = documents.link_of(other);
					const document* other_doc = documents.find(other);
					return other != id && other_context && other_context->kind == link_kind::instance && other_context->parent == context->parent
						&& other_doc && other_doc->title() == row.label;
				};
				std::vector<document_id> siblings = documents.descendants_of(context->parent);
				if (std::ranges::any_of(siblings, same_title)) row.label = std::format("{} ({})", row.label, context->anchor);
			}
			rows.push_back(std::move(row));
		}
		if (!rows.empty()) rows.back().current = true;
		return rows;
	}

	host_id apply_crumb_drop(document_manager& documents, document_id link, const crumb_drop& drop)
	{
		if (!documents.link_of(link)) return 0;
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
