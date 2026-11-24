#pragma once
#include "edt_builtin_asset_kinds.h"
#include "edt_document_cast.h"
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_services.h>
#include <vector>

namespace test_util
{
	// The active prefab document, else the one opened last. Tests only: "the prefab the editor window shows" is the prefab editor
	// component's own state (#1185), which this is not.
	inline edt::prefab_document* prefab_of(edt::document_manager& documents)
	{
		edt::prefab_document* last = nullptr;
		const edt::document* active = documents.active();
		for (edt::prefab_document* prefab : edt::prefabs_of(documents)) {
			if (active && prefab->id() == active->id()) return prefab;
			last = prefab;
		}
		return last;
	}

	// What a standalone document_manager needs to open a prefab preview: the built-in "prefab" kind in a registry of its own.
	// Declare it after the manager and the services it refers to.
	struct prefab_preview_kinds
	{
		prefab_preview_kinds(edt::document_manager& documents, edt::editor_services& services)
		{
			registrations = edt::register_builtin_asset_kinds(kinds, {});
			documents.set_preview_kinds(kinds, services);
		}

		edt::asset_kind_registry kinds;
		std::vector<edt::registration> registrations;
	};
}
