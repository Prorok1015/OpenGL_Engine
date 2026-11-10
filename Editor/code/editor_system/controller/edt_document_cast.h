#pragma once
#include "edt_prefab_document.h"
#include "edt_builtin_asset_kinds.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_document_manager.h>
#include <snk/common/engine_assert.h>
#include <memory>

namespace edt
{
	// The document_manager knows documents only as `document`; the editor knows which ones it put in. The casts are
	// checked: another kind of document is a bug of the caller, and a dynamic_cast to a reference throws bad_cast
	// where a static_cast would be undefined behaviour.
	inline scene_document& scene_of(document_manager& documents) { return dynamic_cast<scene_document&>(documents.scene()); }
	inline const scene_document& scene_of(const document_manager& documents) { return dynamic_cast<const scene_document&>(documents.scene()); }
	// Null when no preview is open (or it is not a prefab).
	inline prefab_document* prefab_of(document_manager& documents) { return dynamic_cast<prefab_document*>(documents.preview()); }
	// By the id of the built-in prefab preview (register_builtin_asset_kinds), not by suffix: a module's preview for a longer
	// suffix (".ui.prefab.desc") must not hand the prefab editor a document that is not a prefab_document.
	inline prefab_document& open_prefab(document_manager& documents, const res::tag& prefab_tag)
	{
		previewable_document* preview = documents.open_preview(k_prefab_preview_id, prefab_tag);
		ASSERT_MSG(preview, "the built-in 'prefab' preview is not registered");
		return dynamic_cast<prefab_document&>(*preview);
	}
}
