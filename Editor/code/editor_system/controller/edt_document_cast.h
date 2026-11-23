#pragma once
#include "edt_prefab_document.h"
#include "edt_builtin_asset_kinds.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_document_manager.h>
#include <snk/common/engine_assert.h>
#include <memory>
#include <vector>
#include <algorithm>

namespace edt
{
	// The document_manager knows documents only as `document`; the editor knows which ones it put in. The casts are
	// checked: another kind of document is a bug of the caller, and a dynamic_cast to a reference throws bad_cast
	// where a static_cast would be undefined behaviour.
	inline scene_document& scene_of(document_manager& documents) { return dynamic_cast<scene_document&>(documents.scene()); }
	inline const scene_document& scene_of(const document_manager& documents) { return dynamic_cast<const scene_document&>(documents.scene()); }
	// The open prefab documents in the order they were opened (a prefab preview is of kind `multiple`, #1185).
	inline std::vector<prefab_document*> prefabs_of(document_manager& documents)
	{
		std::vector<prefab_document*> out;
		for (previewable_document* open : documents.previews()) {
			if (auto* prefab = dynamic_cast<prefab_document*>(open)) out.push_back(prefab);
		}
		return out;
	}
	// The active prefab document, else the one opened last; null when no prefab is open.
	inline prefab_document* prefab_of(document_manager& documents)
	{
		prefab_document* last = nullptr;
		const document* active = documents.active();
		for (prefab_document* prefab : prefabs_of(documents)) {
			if (active && prefab->id() == active->id()) return prefab;
			last = prefab;
		}
		return last;
	}
	// The open prefab documents with unapplied edits (what quit, New, Open and a module reload must not lose).
	inline std::vector<prefab_document*> dirty_prefabs_of(document_manager& documents)
	{
		std::vector<prefab_document*> out = prefabs_of(documents);
		std::erase_if(out, [](const prefab_document* prefab) { return !prefab->is_dirty(); });
		return out;
	}
	// The first of them, else null.
	inline prefab_document* dirty_prefab_of(document_manager& documents)
	{
		const std::vector<prefab_document*> dirty = dirty_prefabs_of(documents);
		return dirty.empty() ? nullptr : dirty.front();
	}
	// By the id of the built-in prefab preview (register_builtin_asset_kinds), not by suffix: a module's preview for a longer
	// suffix (".ui.prefab.desc") must not hand the prefab editor a document that is not a prefab_document.
	inline prefab_document& open_prefab(document_manager& documents, const res::tag& prefab_tag)
	{
		previewable_document* preview = documents.open_preview(k_prefab_preview_id, prefab_tag);
		ASSERT_MSG(preview, "the built-in 'prefab' preview is not registered");
		return dynamic_cast<prefab_document&>(*preview);
	}
}
