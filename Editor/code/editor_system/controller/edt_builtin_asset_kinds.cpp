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
		.make_preview = [layout = hooks.desc_layout, locked = hooks.edits_locked](editor_services& services) -> std::unique_ptr<previewable_document> {
			auto document = std::make_unique<prefab_document>(services);
			if (layout) document->set_files(layout());
			document->set_lock_source(locked);
			return document;
		},
		.multiple = true,
	}));
	// Not "shader": asset_type_of already names GLSL sources so, and an agent's asset.query by kind would mix the two.
	if (hooks.open_desc) {
		out.push_back(registry.add({
			.id = "shader_desc",
			.suffix = ".shader.desc",
			.icon = "[S]",
			.open = [open = std::move(hooks.open_desc)](editor_context&, const res::tag& tag) { open(tag); },
		}));
	}
	// Empty suffix: every dropped asset is added through its desc, as before the registry existed.
	out.push_back(registry.add({
		.id = "desc",
		.drop_into_scene = [add = std::move(hooks.add_desc_to_scene)](editor_context&, const res::tag& tag) { add(tag); },
	}));
	return out;
}

std::vector<std::pair<std::string, std::string>> edt::desc_kind_suffixes(const asset_kind_registry& registry)
{
	std::vector<std::pair<std::string, std::string>> out;
	for (const std::string& id : registry.ids()) {
		const asset_kind_desc* kind = registry.find(id);
		if (kind && kind->suffix.ends_with(".desc")) {
			out.emplace_back(id, kind->suffix);
		}
	}
	return out;
}
