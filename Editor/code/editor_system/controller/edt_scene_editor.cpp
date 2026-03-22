#include "edt_scene_editor.h"
#include "edt_shared_state.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"
#include "scn_model.h"
#include "eng_transform_3d.hpp"
#include <entt/entt.hpp>
#include <algorithm>
#include <unordered_map>

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

	if (type_name != "Empty" && !type_name.empty()) {
		scn::prefab_desc::prefab_comp_node comp;
		comp.type_name = type_name;
		node.components[type_name] = std::move(comp);
	}

	if (on_before_modify) on_before_modify();
	m_state.active_world_desc().get_root().children.push_back(std::move(node));
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::create_entity_from_node(prefab_node node)
{
	if (on_before_modify) on_before_modify();
	m_state.active_world_desc().get_root().children.push_back(std::move(node));
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::delete_entity(const std::string& name)
{
	if (on_before_modify) on_before_modify();
	remove_node_by_name(m_state.active_world_desc().get_root().children, name);
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::delete_entities(const std::vector<std::string>& names)
{
	if (names.empty()) return;
	if (on_before_modify) on_before_modify();
	auto& root_children = m_state.active_world_desc().get_root().children;
	for (const auto& name : names)
		remove_node_by_name(root_children, name);
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::duplicate_entity(const std::string& name)
{
	auto* siblings = find_parent_children(m_state.active_world_desc().get_root().children, name);
	if (!siblings) return;

	auto it = std::find_if(siblings->begin(), siblings->end(),
		[&](const prefab_node& n) { return n.name == name; });
	if (it == siblings->end()) return;

	prefab_node copy = *it;
	copy.name = make_unique_key(name + "_copy");

	if (on_before_modify) on_before_modify();
	siblings->insert(it + 1, std::move(copy));
	m_state.is_dirty = true;
	serialize_and_push();
}

void edt::scene_editor::duplicate_entities(const std::vector<std::string>& names)
{
	if (names.empty()) return;
	if (on_before_modify) on_before_modify();
	auto& root_children = m_state.active_world_desc().get_root().children;
	// Collect copies before any insertions to avoid iterator invalidation
	struct pending_copy { std::vector<prefab_node>* siblings; std::size_t insert_after_idx; prefab_node node; };
	std::vector<pending_copy> copies;
	for (const auto& name : names) {
		auto* siblings = find_parent_children(root_children, name);
		if (!siblings) continue;
		auto it = std::find_if(siblings->begin(), siblings->end(),
			[&](const prefab_node& n) { return n.name == name; });
		if (it == siblings->end()) continue;
		prefab_node copy = *it;
		copy.name = make_unique_key(name + "_copy");
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

void edt::scene_editor::reparent_entity(const std::string& node_name, const std::string& new_parent_name, int insert_index)
{
	auto& root_children = m_state.active_world_desc().get_root().children;

	auto* current_siblings = find_parent_children(root_children, node_name);
	if (!current_siblings) return;

	auto* dragged = find_node_by_name(root_children, node_name);
	if (!dragged) return;
	if (!new_parent_name.empty() && find_node_by_name(dragged->children, new_parent_name))
		return;

	auto src_it = std::find_if(current_siblings->begin(), current_siblings->end(),
		[&](const prefab_node& n) { return n.name == node_name; });
	if (src_it == current_siblings->end()) return;

	prefab_node moved = std::move(*src_it);

	if (on_before_modify) on_before_modify();
	current_siblings->erase(src_it);

	if (insert_index == -1) {
		if (new_parent_name.empty()) {
			root_children.push_back(std::move(moved));
		} else {
			auto* target = find_node_by_name(root_children, new_parent_name);
			if (target)
				target->children.push_back(std::move(moved));
			else
				root_children.push_back(std::move(moved));
		}
	} else {
		auto* target_siblings = find_parent_children(root_children, new_parent_name);
		if (!target_siblings) {
			root_children.push_back(std::move(moved));
		} else {
			auto dst_it = std::find_if(target_siblings->begin(), target_siblings->end(),
				[&](const prefab_node& n) { return n.name == new_parent_name; });
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
	const std::string& name, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale)
{
	auto* node = find_node_by_name(m_state.active_world_desc().get_root().children, name);
	if (!node) return;
	node->position = pos;
	node->rotation = rot;
	node->scale    = scale;
	m_state.is_dirty = true;
	serialize_and_push();
}

// ─── Desc ↔ ECS sync ────────────────────────────────────────────────────────

void edt::scene_editor::serialize_and_push()
{
	if (!m_state.editor_tag.is_valid()) return;
	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr || !m_state.sfactory || !m_state.assembler) return;

	const std::string world_name = m_state.active_world_idx < (int)m_state.world_names.size()
		? m_state.world_names[m_state.active_world_idx]
		: std::string(m_state.active_world_desc().get_name());

	scn::level& lvl = lvl_mgr->get_level();
	if (!lvl.has_world(world_name))
		return;

	{
		scn::world& world = lvl.get_world(world_name);
		if (on_save_camera) on_save_camera(world.state());
	}
	// TODO: Better just save the world on a disk. hot reload system will take care of reloading it in the editor and the game.
	lvl.reload_world(world_name, m_state.active_world_desc(), *m_state.sfactory, *m_state.assembler);

	scn::world& world = lvl.get_world(world_name);
	if (on_inject_camera) on_inject_camera(world.state());

	if (on_after_push) on_after_push();
}

static void build_node_map(
	std::vector<scn::prefab_desc::prefab_node>& nodes,
	std::unordered_map<std::string_view, scn::prefab_desc::prefab_node*>& out)
{
	for (auto& n : nodes) {
		out[n.name] = &n;
		build_node_map(n.children, out);
	}
}

void edt::scene_editor::sync_ecs_transforms_to_desc()
{
	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr || m_state.active_world_idx >= (int)m_state.world_names.size()) return;

	std::unordered_map<std::string_view, prefab_node*> node_map;
	build_node_map(m_state.active_world_desc().get_root().children, node_map);

	auto& reg = lvl_mgr->get_level().get_world(m_state.world_names[m_state.active_world_idx]).state();
	reg.view<scn::name_component, scn::local_transform>().each(
		[&node_map](const scn::name_component& nc, const scn::local_transform& lt) {
			auto it = node_map.find(nc.name);
			if (it == node_map.end()) return;
			auto* node = it->second;
			eng::transform3d tr{ lt.local };
			node->position = tr.get_pos();
			node->rotation = tr.get_angles_degrees();
			node->scale    = tr.get_scale();
		});
}

// ─── Desc tree helpers ───────────────────────────────────────────────────────

std::string edt::scene_editor::make_unique_key(const std::string& base_name) const
{
	const auto& children = m_state.active_world_desc().get_root().children;
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

edt::scene_editor::prefab_node* edt::scene_editor::find_node_by_name(
	std::vector<prefab_node>& nodes, const std::string& name)
{
	for (auto& node : nodes) {
		if (node.name == name)
			return &node;
		if (auto* found = find_node_by_name(node.children, name))
			return found;
	}
	return nullptr;
}

bool edt::scene_editor::remove_node_by_name(
	std::vector<prefab_node>& nodes, const std::string& name)
{
	for (auto it = nodes.begin(); it != nodes.end(); ++it) {
		if (it->name == name) {
			nodes.erase(it);
			return true;
		}
		if (remove_node_by_name(it->children, name))
			return true;
	}
	return false;
}

std::vector<edt::scene_editor::prefab_node>* edt::scene_editor::find_parent_children(
	std::vector<prefab_node>& nodes, const std::string& name)
{
	for (auto& node : nodes) {
		if (node.name == name)
			return &nodes;
		if (auto* p = find_parent_children(node.children, name))
			return p;
	}
	return nullptr;
}
