#include "edt_node_patch.h"
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/common/logger/engine_log.h>
#include <exception>
#include <string>
#include <vector>

namespace {

	using prefab_node = scn::prefab_desc::prefab_node;

	bool path_to(const prefab_node& node, std::uint64_t id, std::vector<std::uint64_t>& path, const prefab_node*& found)
	{
		for (const auto& child : node.children) {
			path.push_back(child.node_id);
			if (child.node_id == id) {
				found = &child;
				return true;
			}
			if (path_to(child, id, path, found)) {
				return true;
			}
			path.pop_back();
		}
		return false;
	}

	struct target
	{
		entt::entity entity = entt::null;
		const prefab_node* node = nullptr;
		const edt::scene_node_edit* edit = nullptr;
	};

}

bool edt::patch_live_nodes(const scn::world_desc& desc, entt::registry& reg, const scn::ecs_assembler& assembler,
	std::span<const scene_node_edit> edits)
{
	std::vector<target> targets;
	targets.reserve(edits.size());
	const scn::node_index& index = scn::node_index::get(reg);
	for (const auto& edit : edits) {
		std::vector<std::uint64_t> path;
		const prefab_node* node = nullptr;
		if (!path_to(desc.get_root(), edit.id, path, node)) {
			return false;
		}
		const entt::entity entity = index.find(path);
		if (entity == entt::null || !reg.valid(entity)) {
			return false;
		}
		for (const auto& name : edit.components) {
			if (!node->components.contains(name)) {
				return false;
			}
		}
		targets.push_back({ entity, node, &edit });
	}

	try {
		for (const auto& [entity, node, edit] : targets) {
			if (edit->transform) {
				reg.get_or_emplace<scn::local_transform>(entity).local = node->get_local_transform();
				if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
					reg.ctx().get<ecs::event<scn::transform_updated>>().emit(entity);
				}
			}
			if (edit->name) {
				const std::string name = node->name.empty() ? std::string{ "unnamed_child" } : node->name;
				reg.emplace_or_replace<scn::name_component>(entity, scn::name_component{ .name = name });
			}
			for (const auto& comp_name : edit->components) {
				const auto& component = node->components.at(comp_name);
				assembler.assemble_and_apply(reg, entity, component.type_name, component.parent_desc, component.overrides, comp_name);
			}
		}
	} catch (const std::exception& error) {
		egLOG_WARN("Editor/Patch", "a node patch failed, the world is rebuilt: {}", error.what());
		return false;
	}
	return true;
}
