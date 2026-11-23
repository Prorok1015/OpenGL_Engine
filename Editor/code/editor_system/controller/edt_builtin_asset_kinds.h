#pragma once
#include "edt_desc_file_commands.h"
#include <snk/editor/edt_asset_kind_registry.h>
#include <functional>
#include <string_view>
#include <vector>

namespace edt
{
	// What the built-in asset kinds need from the rest of the editor. Plain callbacks, so the kinds can be
	// registered and dispatched without a level or ImGui.
	struct asset_kind_hooks
	{
		std::function<void(const res::tag&)> open_prefab;
		// Loads the desc and adds it to the active world as a new node.
		std::function<void(const res::tag&)> add_desc_to_scene;
		// Where the project's desc files are (what an Apply of the prefab checks before it writes); null: a prefab cannot be
		// written. The members have initializers: a test that builds the hooks with only the first two keeps compiling.
		std::function<desc_file_layout()> desc_layout = {};
		// Play runs: the prefab cannot be edited or applied. Null: never.
		std::function<bool()> edits_locked = {};
		// Opens a desc file as a data document (the schema inspector). Null: the "shader" kind opens nothing.
		std::function<void(const res::tag&)> open_desc = {};
	};

	// The id of the preview that makes the prefab_document (open_prefab names it, so a module's preview with a longer suffix
	// cannot take its place).
	inline constexpr std::string_view k_prefab_preview_id = "prefab";

	// "prefab" (.prefab.desc: double-click opens the prefab editor; its preview is the prefab_document, registered as an
	// asset preview with the same id), "shader" (.shader.desc: double-click opens it as a data document) and "desc" (any
	// asset dropped onto the viewport is added to the scene). The kinds stay registered while the returned handles live.
	[[nodiscard]] std::vector<registration> register_builtin_asset_kinds(asset_kind_registry& registry, asset_kind_hooks hooks);
}
