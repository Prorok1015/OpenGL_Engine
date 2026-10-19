#pragma once
#include <snk/editor/edt_asset_kind_registry.h>
#include <functional>
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
	};

	// "prefab" (.prefab.desc: double-click opens the prefab editor) and "desc" (any asset dropped onto the
	// viewport is added to the scene). The kinds stay registered while the returned handles live.
	[[nodiscard]] std::vector<registration> register_builtin_asset_kinds(asset_kind_registry& registry, asset_kind_hooks hooks);
}
