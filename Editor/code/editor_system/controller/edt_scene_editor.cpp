#include "edt_scene_editor.h"
#include "edt_shared_state.h"
#include "edt_override_edit.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"
#include "level/scn_node_index.h"
#include "scn_model.h"
#include "desc_node_id.h"
#include "logger/engine_log.h"
#include "eng_transform_3d.hpp"
#include <entt/entt.hpp>
#include <algorithm>

edt::scene_editor::scene_editor(shared_state& state)
	: m_state(state)
{
}

// ─── Entity CRUD ─────────────────────────────────────────────────────────────

void edt::scene_editor::create_entity(const std::string& type_name)
{
	std::string base = (type_name == "Empty" || type_name.empty()) ? "Entity" : type_name;
	std::string key  = make_unique_key(base);

	prefab_node node;
	node.name = key;
	node.node_id = desc::new_node_id();

	if (type_name != "Empty" && !type_name.empty()) {
		scn::prefab_desc::prefab_comp_node comp;
		comp.type_name = type_name;
		node.components[type_name] = std::move(comp);
	}

	auto* world = m_state.active_world_desc();
	if (!world) return;
	if (on_before_modify) on_before_modify();
	world->get_root().children.push_back(std::move(node));
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::create_entity_from_node(prefab_node node)
{
	if (node.node_id == desc::invalid_node_id)
		node.node_id = desc::new_node_id();
	auto* world = m_state.active_world_desc();
	if (!world) return;
	if (on_before_modify) on_before_modify();
	world->get_root().children.push_back(std::move(node));
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::delete_entity(uint64_t node_id)
{
	auto* world = m_state.active_world_desc();
	if (!world) return;
	if (on_before_modify) on_before_modify();
	remove_node_by_id(world->get_root().children, node_id);
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::delete_entities(const std::vector<uint64_t>& node_ids)
{
	auto* world = m_state.active_world_desc();
	if (node_ids.empty() || !world) return;
	if (on_before_modify) on_before_modify();
	auto& root_children = world->get_root().children;
	for (uint64_t id : node_ids)
		remove_node_by_id(root_children, id);
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::duplicate_entity(uint64_t node_id)
{
	auto* world = m_state.active_world_desc();
	if (!world) return;
	auto* siblings = find_parent_children(world->get_root().children, node_id);
	if (!siblings) return;

	auto it = std::find_if(siblings->begin(), siblings->end(),
		[&](const prefab_node& n) { return n.node_id == node_id; });
	if (it == siblings->end()) return;

	prefab_node copy = *it;
	copy.name = make_unique_key(copy.name + "_copy");
	scn::regenerate_node_ids(copy);

	if (on_before_modify) on_before_modify();
	siblings->insert(it + 1, std::move(copy));
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::duplicate_entities(const std::vector<uint64_t>& node_ids)
{
	auto* world = m_state.active_world_desc();
	if (node_ids.empty() || !world) return;
	if (on_before_modify) on_before_modify();
	auto& root_children = world->get_root().children;
	// Collect copies before any insertions to avoid iterator invalidation
	struct pending_copy { std::vector<prefab_node>* siblings; std::size_t insert_after_idx; prefab_node node; };
	std::vector<pending_copy> copies;
	for (uint64_t id : node_ids) {
		auto* siblings = find_parent_children(root_children, id);
		if (!siblings) continue;
		auto it = std::find_if(siblings->begin(), siblings->end(),
			[&](const prefab_node& n) { return n.node_id == id; });
		if (it == siblings->end()) continue;
		prefab_node copy = *it;
		copy.name = make_unique_key(copy.name + "_copy");
		scn::regenerate_node_ids(copy);
		std::size_t idx = static_cast<std::size_t>(std::distance(siblings->begin(), it));
		copies.push_back({ siblings, idx + 1, std::move(copy) });
	}
	// Insert from highest index to lowest within each sibling list to preserve indices
	std::stable_sort(copies.begin(), copies.end(), [](const pending_copy& a, const pending_copy& b) {
		if (a.siblings != b.siblings) return a.siblings > b.siblings;
		return a.insert_after_idx > b.insert_after_idx;
	});
	for (auto& c : copies)
		c.siblings->insert(c.siblings->begin() + static_cast<std::ptrdiff_t>(c.insert_after_idx), std::move(c.node));
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::reparent_entity(uint64_t node_id, uint64_t new_parent_id, int insert_index)
{
	auto* world = m_state.active_world_desc();
	if (!world) return;
	auto& root_children = world->get_root().children;

	auto* current_siblings = find_parent_children(root_children, node_id);
	if (!current_siblings) return;

	auto* dragged = find_node_by_id(root_children, node_id);
	if (!dragged) return;
	if (new_parent_id == node_id) return;
	if (new_parent_id != desc::invalid_node_id && find_node_by_id(dragged->children, new_parent_id))
		return;

	auto src_it = std::find_if(current_siblings->begin(), current_siblings->end(),
		[&](const prefab_node& n) { return n.node_id == node_id; });
	if (src_it == current_siblings->end()) return;

	prefab_node moved = std::move(*src_it);

	if (on_before_modify) on_before_modify();
	current_siblings->erase(src_it);

	if (insert_index == -1) {
		if (new_parent_id == desc::invalid_node_id) {
			root_children.push_back(std::move(moved));
		} else {
			auto* target = find_node_by_id(root_children, new_parent_id);
			if (target)
				target->children.push_back(std::move(moved));
			else
				root_children.push_back(std::move(moved));
		}
	} else {
		auto* target_siblings = find_parent_children(root_children, new_parent_id);
		if (!target_siblings) {
			root_children.push_back(std::move(moved));
		} else {
			auto dst_it = std::find_if(target_siblings->begin(), target_siblings->end(),
				[&](const prefab_node& n) { return n.node_id == new_parent_id; });
			if (dst_it != target_siblings->end()) {
				if (insert_index == 1) ++dst_it;
				target_siblings->insert(dst_it, std::move(moved));
			} else {
				target_siblings->push_back(std::move(moved));
			}
		}
	}

	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::on_transform_committed(
	std::span<const uint64_t> path, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale)
{
	auto* world = m_state.active_world_desc();
	if (!world) return;
	auto address = resolve_node_path(world->get_root().children, path);
	if (!address.owner) return;

	if (address.target.empty()) {
		auto& node = *address.owner;
		node.position = pos;
		node.rotation = rot;
		node.scale    = scale;
		node.has_transform = true;
	} else {
		if (!m_state.assembler) return;
		const auto base = override_edit::find_base_node(*m_state.assembler, *address.owner, address.target);
		if (!base) {
			egLOG_WARN("editor", "no node below '{}' for the committed transform", address.owner->name);
			return;
		}
		override_edit::write_transform(*address.owner, address.target, *base, pos, rot, scale);
	}
	m_state.is_dirty = true;
	serialize_and_push();
}

// ─── Desc ↔ ECS sync ────────────────────────────────────────────────────────

void edt::scene_editor::serialize_and_push()
{
	if (!m_state.editor_tag.is_valid()) return;
	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr || !m_state.sfactory || !m_state.assembler) return;

	auto* world_desc = m_state.active_world_desc();
	if (!world_desc) return;
	const std::string world_name = m_state.active_world_name()
		? *m_state.active_world_name()
		: std::string(world_desc->get_name());

	scn::level& lvl = lvl_mgr->get_level();
	if (!lvl.has_world(world_name))
		return;

	{
		scn::world& world = lvl.get_world(world_name);
		if (on_save_camera) on_save_camera(world.state());
	}
	// TODO: Better just save the world on a disk. hot reload system will take care of reloading it in the editor and the game.
	lvl.reload_world(world_name, *world_desc, *m_state.sfactory, *m_state.assembler);

	scn::world& world = lvl.get_world(world_name);
	if (on_inject_camera) on_inject_camera(world.state());

	if (on_after_push) on_after_push();
}

void edt::scene_editor::sync_ecs_transforms_to_desc()
{
	auto lvl_mgr = m_state.lvl_manager.lock();
	auto* world_desc = m_state.active_world_desc();
	const std::string* world_name = m_state.active_world_name();
	if (!lvl_mgr || !world_desc || !world_name) return;

	auto& children = world_desc->get_root().children;
	auto& reg = lvl_mgr->get_level().get_world(*world_name).state();
	reg.view<scn::node_id_component, scn::local_transform>().each(
		[&children](const scn::node_id_component& id, const scn::local_transform& lt) {
			if (id.path.empty()) return;
			// A node inside an instance is placed by its override records, not by the live transform.
			auto address = resolve_node_path(children, id.path);
			if (!address.owner || !address.target.empty()) return;
			write_live_transform(*address.owner, lt.local);
		});
}

void edt::scene_editor::write_live_transform(prefab_node& node, const glm::mat4& local)
{
	const bool references_desc = std::ranges::any_of(node.components, [] (const auto& entry) { return entry.second.parent_desc.is_valid(); });
	if (references_desc && !node.has_transform) {
		return;
	}
	eng::transform3d tr{ local };
	node.position = tr.get_pos();
	node.rotation = tr.get_angles_degrees();
	node.scale    = tr.get_scale();
}

// ─── Desc tree helpers ───────────────────────────────────────────────────────

std::string edt::scene_editor::make_unique_key(const std::string& base_name) const
{
	const auto* world = m_state.active_world_desc();
	if (!world) return base_name;
	const auto& children = world->get_root().children;
	auto has_name = [&](const std::string& n) {
		for (const auto& c : children)
			if (c.name == n) return true;
		return false;
	};
	if (!has_name(base_name))
		return base_name;
	for (int i = 1; ; ++i) {
		std::string candidate = base_name + "_" + std::to_string(i);
		if (!has_name(candidate))
			return candidate;
	}
}

edt::scene_editor::prefab_node* edt::scene_editor::find_node_by_id(
	std::vector<prefab_node>& nodes, uint64_t node_id)
{
	if (node_id == desc::invalid_node_id) return nullptr;
	for (auto& node : nodes) {
		if (node.node_id == node_id)
			return &node;
		if (auto* found = find_node_by_id(node.children, node_id))
			return found;
	}
	return nullptr;
}

bool edt::scene_editor::remove_node_by_id(
	std::vector<prefab_node>& nodes, uint64_t node_id)
{
	if (node_id == desc::invalid_node_id) return false;
	for (auto it = nodes.begin(); it != nodes.end(); ++it) {
		if (it->node_id == node_id) {
			nodes.erase(it);
			return true;
		}
		if (remove_node_by_id(it->children, node_id))
			return true;
	}
	return false;
}

std::vector<edt::scene_editor::prefab_node>* edt::scene_editor::find_parent_children(
	std::vector<prefab_node>& nodes, uint64_t node_id)
{
	if (node_id == desc::invalid_node_id) return nullptr;
	for (auto& node : nodes) {
		if (node.node_id == node_id)
			return &nodes;
		if (auto* p = find_parent_children(node.children, node_id))
			return p;
	}
	return nullptr;
}

edt::scene_editor::node_address edt::scene_editor::resolve_node_path(
	std::vector<prefab_node>& nodes, std::span<const uint64_t> path)
{
	node_address address;
	std::vector<prefab_node>* level = &nodes;
	for (std::size_t i = 0; i < path.size(); ++i) {
		auto it = std::ranges::find(*level, path[i], &prefab_node::node_id);
		if (it == level->end()) {
			if (address.owner)
				address.target.assign(path.begin() + static_cast<std::ptrdiff_t>(i), path.end());
			return address;
		}
		address.owner = &*it;
		level = &it->children;
	}
	return address;
}
