#pragma once
#include "edt_builtin_asset_kinds.h"
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_services.h>
#include <vector>

namespace test_util
{
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
