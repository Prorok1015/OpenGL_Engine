#include "edt_builtin_asset_kinds.h"
#include "edt_prefab_document.h"
#include <memory>
#include <string>
#include <utility>

std::vector<edt::registration> edt::register_builtin_asset_kinds(asset_kind_registry& registry, asset_kind_hooks hooks)
{
	std::vector<registration> out;
	out.reserve(4);
	out.push_back(registry.add({
		.id = "prefab",
		.suffix = ".prefab.desc",
		.icon = "[D]",
		.open = [open = hooks.open_prefab](editor_context&, const res::tag& tag) { open(tag); },
	}));
	out.push_back(registry.add_preview({
		.id = std::string(k_prefab_preview_id),
		.suffix = ".prefab.desc",
		.make_preview = [](editor_services& services) -> std::unique_ptr<previewable_document> { return std::make_unique<prefab_document>(services); },
	}));
	out.push_back(registry.add({
		.id = "data_document",
		.suffix = ".desc",
		.open = [open = hooks.open_data_document](editor_context&, const res::tag& tag) { if (open) open(tag); },
	}));
	// Empty suffix: every dropped asset is added through its desc, as before the registry existed.
	out.push_back(registry.add({
		.id = "desc",
		.drop_into_scene = [add = std::move(hooks.add_desc_to_scene)](editor_context&, const res::tag& tag) { add(tag); },
	}));
	return out;
}
