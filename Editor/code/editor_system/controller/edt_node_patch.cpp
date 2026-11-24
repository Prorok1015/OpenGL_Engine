#include "edt_node_patch.h"
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

	using prefab_node = scn::prefab_desc::prefab_node;

	struct located
	{
		std::vector<std::uint64_t> path;
		const prefab_node* node = nullptr;
		bool overridden = false; // the node or an ancestor has override records
	};

	void locate_all(const prefab_node& node, std::vector<std::uint64_t>& path, bool overridden,
		std::unordered_map<std::uint64_t, located>& out)
	{
		for (const auto& child : node.children) {
			path.push_back(child.node_id);
			const bool below_records = overridden || !child.overrides.empty();
			out.emplace(child.node_id, located{ path, &child, below_records });
			locate_all(child, path, below_records, out);
			path.pop_back();
		}
	}

	// The engine types whose spawner replaces its components from the desc alone and reads nothing else. A schema component
	// (a module's) registers a patcher, which is the same promise. Everything else (a mesh node, skinning, animation...) can
	// leave stale components behind when a field is cleared, or reads other nodes: it is rebuilt until it is added here on purpose.
	constexpr std::array<std::string_view, 2> k_patchable_engine_types{ "camera_desc", "directional_light_desc" };

	bool patchable_type(const scn::ecs_assembler& assembler, std::string_view type)
	{
		if (type.empty()) {
			return false;
		}
		if (std::ranges::find(k_patchable_engine_types, type) != k_patchable_engine_types.end()) {
			return true;
		}
		const auto* spawner = assembler.find_spawner(type);
		return spawner && spawner->patch && spawner->category == scn::spawner_category::component;
	}

	// find_prefab_ref's rule: a component that may name a prefab and has a parent desc makes the node an instance, whose
	// transform and content are placed by the prefab and its records, not by this node alone.
	bool names_a_prefab(const scn::prefab_desc::prefab_comp_node& component)
	{
		const bool may_be_prefab = component.type_name.empty() || component.type_name == "prefab_desc" || component.type_name == "world_desc";
		return may_be_prefab && component.parent_desc.is_valid();
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
	std::unordered_map<std::uint64_t, located> nodes;
	std::vector<std::uint64_t> path;
	if (!desc.get_root().overrides.empty()) {
		return false;
	}
	locate_all(desc.get_root(), path, false, nodes);

	std::vector<target> targets;
	targets.reserve(edits.size());
	const scn::node_index& index = scn::node_index::get(reg);
	for (const auto& edit : edits) {
		const auto found = nodes.find(edit.id);
		if (found == nodes.end() || found->second.overridden) {
			return false;
		}
		const prefab_node& node = *found->second.node;
		for (const auto& [name, component] : node.components) {
			if (names_a_prefab(component)) {
				return false;
			}
		}
		for (const auto& name : edit.components) {
			const auto component = node.components.find(name);
			if (component == node.components.end() || !patchable_type(assembler, component->second.type_name)
				|| (component->second.parent_desc.is_valid() && component->second.parent_desc.has_error())) {
				return false;
			}
		}
		const entt::entity entity = index.find(found->second.path);
		if (entity == entt::null || !reg.valid(entity)) {
			return false;
		}
		targets.push_back({ entity, &node, &edit });
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
