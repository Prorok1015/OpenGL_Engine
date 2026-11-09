#pragma once
#include "edt_prefab_document.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_document_manager.h>
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
	// What the document_manager builds its preview with.
	inline preview_factory prefab_preview(editor_services& services)
	{
		return [&services] { return std::make_unique<prefab_document>(services); };
	}
	inline prefab_document& open_prefab(document_manager& documents, const res::tag& prefab_tag)
	{
		return dynamic_cast<prefab_document&>(documents.open_preview(prefab_tag));
	}
}
