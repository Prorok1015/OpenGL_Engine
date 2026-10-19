#include "edt_builtin_asset_kinds.h"
#include <utility>

std::vector<edt::registration> edt::register_builtin_asset_kinds(asset_kind_registry& registry, asset_kind_hooks hooks)
{
	std::vector<registration> out;
	out.reserve(2);
	out.push_back(registry.add({
		.id = "prefab",
		.suffix = ".prefab.desc",
		.icon = "[D]",
		.open = [open = hooks.open_prefab](editor_context&, const res::tag& tag) { open(tag); },
	}));
	// Empty suffix: every dropped asset is added through its desc, as before the registry existed.
	out.push_back(registry.add({
		.id = "desc",
		.drop_into_scene = [add = std::move(hooks.add_desc_to_scene)](editor_context&, const res::tag& tag) { add(tag); },
	}));
	return out;
}
