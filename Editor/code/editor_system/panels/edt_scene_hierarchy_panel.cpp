#include "edt_scene_hierarchy_panel.h"
#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_directional_light_desc.h>
#include <snk/scene/scn_mesh_node_desc.h>
#include <snk/scene/scn_skybox_desc.h>
#include <snk/scene/scn_bone_desc.h>
#include "edt_override_edit.h"
#include <imgui.h>
#include <string>
#include <algorithm>
#include <cctype>
#include <format>
#include <cstring>

namespace edt
{
	scene_hierarchy_panel::scene_hierarchy_panel()
		: panel_base("scene_hierarchy", "Scene Hierarchy")
	{
	}

	void scene_hierarchy_panel::set_view_cache(std::shared_ptr<override_edit::view_cache> cache)
	{
		m_view_cache = std::move(cache);
	}

	void scene_hierarchy_panel::set_world_desc(scn::world_desc* desc)
	{
		m_world_desc    = desc;
		m_selected_node = nullptr;
		m_selected_owner = nullptr;
		m_selected_path.clear();
		m_selected_target.clear();
		m_rename_node   = nullptr;
		m_multi_selected.clear();
		m_last_clicked_flat_idx = -1;
	}

	void scene_hierarchy_panel::set_selected_node(scn::prefab_desc::prefab_node* node)
	{
		m_selected_node = node;
		m_selected_readonly = false;
		m_selected_owner = nullptr;
		m_selected_target.clear();
		m_selected_path.clear();
		if (node && m_world_desc) find_path(m_world_desc->get_root().children, node, m_selected_path);
		m_multi_selected.clear();
		m_last_clicked_flat_idx = -1;
	}

	void scene_hierarchy_panel::set_on_node_selected(std::function<void(scn::prefab_desc::prefab_node*)> cb)
	{
		m_on_node_selected = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_delete_node(std::function<void(uint64_t)> cb)
	{
		m_on_delete_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_delete_nodes(std::function<void(const std::vector<uint64_t>&)> cb)
	{
		m_on_delete_nodes = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_create_node(std::function<void(const std::string&)> cb)
	{
		m_on_create_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_rename_node(std::function<void(const std::string&, const std::string&)> cb)
	{
		m_on_rename_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_duplicate_node(std::function<void(uint64_t)> cb)
	{
		m_on_duplicate_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_duplicate_nodes(std::function<void(const std::vector<uint64_t>&)> cb)
	{
		m_on_duplicate_nodes = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_reparent_node(std::function<void(uint64_t, uint64_t, int)> cb)
	{
		m_on_reparent_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_world_names(std::vector<std::string> names, int active_idx)
	{
		bool changed = (m_world_idx != active_idx) || (m_world_names != names);
		m_world_names = std::move(names);
		m_world_idx   = active_idx;
		if (changed)
			m_force_tab_switch = true;
	}

	// ─── helpers ──────────────────────────────────────────────────────────────

	bool scene_hierarchy_panel::node_matches_filter(const scn::prefab_desc::prefab_node& node, const std::string& lower_filter) const
	{
		std::string lower_name = node.name;
		std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
		if (lower_name.find(lower_filter) != std::string::npos)
			return true;
		for (const auto& child : node.children)
			if (node_matches_filter(child, lower_filter)) return true;
		return false;
	}

	const char* scene_hierarchy_panel::get_type_prefix(const scn::prefab_desc::prefab_node& node) const
	{
		if (node.components.count(std::string(scn::camera_desc::__type)))             return "[C] ";
		if (node.components.count(std::string(scn::directional_light_desc::__type)))  return "[L] ";
		if (node.components.count(std::string(scn::mesh_node_desc::__type)))          return "[M] ";
		if (node.components.count(std::string(scn::skybox_desc::__type)))             return "[S] ";
		if (get_referenced_prefab(node))                                              return "[P] ";
		return "";
	}

	const scn::prefab_desc* scene_hierarchy_panel::get_referenced_prefab(const scn::prefab_desc::prefab_node& node) const
	{
		for (const auto& [key, comp] : node.components) {
			if (!comp.parent_desc.is_valid() || !comp.parent_desc.is_ready()) continue;
			if (comp.parent_desc->get_type() == scn::prefab_desc::__type) {
				auto ptr = comp.parent_desc.get();
				if (ptr) return static_cast<const scn::prefab_desc*>(ptr.get());
			}
		}
		return nullptr;
	}

	bool scene_hierarchy_panel::find_path(
		const std::vector<scn::prefab_desc::prefab_node>& nodes, const scn::prefab_desc::prefab_node* wanted, std::vector<uint64_t>& path) const
	{
		for (const auto& node : nodes) {
			path.push_back(node.node_id);
			if (&node == wanted || find_path(node.children, wanted, path))
				return true;
			path.pop_back();
		}
		return false;
	}

	void scene_hierarchy_panel::notify_selected()
	{
		if (!m_selected_readonly) {
			m_selected_owner = nullptr;
			m_selected_target.clear();
			m_selected_path.clear();
			if (m_selected_node && m_world_desc)
				find_path(m_world_desc->get_root().children, m_selected_node, m_selected_path);
		}
		if (m_on_node_selected) m_on_node_selected(m_selected_node);
	}

	void scene_hierarchy_panel::draw_instance_children(scn::prefab_desc::prefab_node& owner, std::vector<uint64_t>& target)
	{
		constexpr std::size_t max_listed_depth = 16; // a prefab that instantiates itself must not unfold forever
		if (!m_view_cache || target.size() >= max_listed_depth) return;

		// The same composed expansion the inspector edits against: intermediate removes / add_nodes are in it.
		// Held for the loop: a click below invalidates the cache, the listing being drawn must outlive that.
		const auto kids = m_view_cache->children(owner, target);
		for (const auto& kid : *kids) {
			const auto& child = kid.node;
			// The owner's own children are drawn by draw_node already.
			if (target.empty() && std::ranges::any_of(owner.children, [&] (const auto& own) { return own.node_id == child.node_id; }))
				continue;

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
			if (!kid.has_children) flags |= ImGuiTreeNodeFlags_Leaf;

			target.push_back(child.node_id);

			// Highlight if this node of the instantiated prefab is selected
			if (m_selected_readonly && m_selected_owner == &owner && m_selected_target == target)
				flags |= ImGuiTreeNodeFlags_Selected;

			std::string prefix;
			if (child.components.count(std::string(scn::mesh_node_desc::__type)))   prefix = "[M] ";
			else if (child.components.count(std::string(scn::bone_desc::__type)))   prefix = "[B] ";
			else if (child.components.count(std::string(scn::camera_desc::__type))) prefix = "[C] ";

			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
			std::string id = std::format("{}{}##prefab_{:x}", prefix, child.name, child.node_id);
			bool opened = ImGui::TreeNodeEx(id.c_str(), flags);
			ImGui::PopStyleColor();

			// A click selects the node; its edits become override records on the instance
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
				m_multi_selected.clear();
				// A copy: the cached listing is dropped on the next edit, the selection outlives it.
				m_instance_selection = child;
				m_selected_node = &m_instance_selection;
				m_selected_readonly = true;
				m_selected_owner = &owner;
				m_selected_target = target;
				m_selected_path.clear();
				find_path(m_world_desc->get_root().children, &owner, m_selected_path);
				m_selected_path.insert(m_selected_path.end(), target.begin(), target.end());
				if (m_on_node_selected) m_on_node_selected(m_selected_node);
			}

			if (opened) {
				draw_instance_children(owner, target);
				ImGui::TreePop();
			}
			target.pop_back();
		}
	}

	void scene_hierarchy_panel::collect_flat_order(const std::vector<scn::prefab_desc::prefab_node>& nodes, const std::string& lower_filter)
	{
		for (const auto& node : nodes) {
			const bool filter_active = !lower_filter.empty();
			if (filter_active && !node_matches_filter(node, lower_filter))
				continue;
			m_flat_visible_nodes.push_back(const_cast<scn::prefab_desc::prefab_node*>(&node));
			collect_flat_order(node.children, lower_filter);
		}
	}

	void scene_hierarchy_panel::delete_selected()
	{
		if (m_multi_selected.size() > 1) {
			std::vector<uint64_t> ids;
			ids.reserve(m_multi_selected.size());
			for (auto* n : m_multi_selected)
				ids.push_back(n->node_id);
			m_multi_selected.clear();
			m_selected_node = nullptr;
			if (m_on_delete_nodes) m_on_delete_nodes(ids);
		} else if (m_selected_node) {
			const uint64_t id = m_selected_node->node_id;
			m_selected_node = nullptr;
			m_multi_selected.clear();
			if (m_on_delete_node) m_on_delete_node(id);
		}
	}

	void scene_hierarchy_panel::duplicate_selected()
	{
		if (m_multi_selected.size() > 1) {
			std::vector<uint64_t> ids;
			ids.reserve(m_multi_selected.size());
			for (auto* n : m_multi_selected)
				ids.push_back(n->node_id);
			m_multi_selected.clear();
			m_selected_node = nullptr;
			if (m_on_duplicate_nodes) m_on_duplicate_nodes(ids);
		} else if (m_selected_node) {
			const uint64_t id = m_selected_node->node_id;
			m_selected_node = nullptr;
			m_multi_selected.clear();
			if (m_on_duplicate_node) m_on_duplicate_node(id);
		}
	}

	// ─── rendering ────────────────────────────────────────────────────────────

	void scene_hierarchy_panel::draw_world_tabs()
	{
		if (m_world_names.empty())
			return;

		ImGuiTabBarFlags bar_flags = ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton;
		if (ImGui::BeginTabBar("##world_tabs", bar_flags)) {
			for (int i = 0; i < (int)m_world_names.size(); ++i) {
				ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
				if (m_force_tab_switch && i == m_world_idx)
					flags |= ImGuiTabItemFlags_SetSelected;

				if (ImGui::BeginTabItem(m_world_names[i].c_str(), nullptr, flags)) {
					if (i != m_world_idx) {
						m_world_idx     = i;
						m_selected_node = nullptr;
						m_selected_owner = nullptr;
						m_selected_path.clear();
						m_selected_target.clear();
						m_rename_node   = nullptr;
						m_multi_selected.clear();
						m_last_clicked_flat_idx = -1;
						if (m_on_world_changed) m_on_world_changed(i);
					}
					ImGui::EndTabItem();
				}
			}

			if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
				m_new_world_buf[0] = '\0';
				ImGui::OpenPopup("##new_world_popup");
			}

			draw_new_world_popup();

			ImGui::EndTabBar();
		}

		m_force_tab_switch = false;
	}

	void scene_hierarchy_panel::draw_new_world_popup()
	{
		ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_Always);
		if (ImGui::BeginPopup("##new_world_popup")) {
			ImGui::TextUnformatted("New world name:");
			ImGui::SetNextItemWidth(-1.f);
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			bool confirm = ImGui::InputText("##new_world_name", m_new_world_buf, sizeof(m_new_world_buf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

			const bool valid = m_new_world_buf[0] != '\0';
			if (!valid) ImGui::BeginDisabled();
			if (ImGui::Button("Create", ImVec2(-1, 0)) || (confirm && valid)) {
				if (m_on_create_world) m_on_create_world(m_new_world_buf);
				ImGui::CloseCurrentPopup();
			}
			if (!valid) ImGui::EndDisabled();
			ImGui::EndPopup();
		}
	}

	void scene_hierarchy_panel::on_render()
	{
		if (!m_world_names.empty()) {
			draw_world_tabs();
			ImGui::Separator();
		} else {
			// Without worlds there are no tabs, so this is the only way to get one.
			ImGui::TextDisabled("This level has no worlds");
			if (ImGui::Button("+ New World")) {
				m_new_world_buf[0] = '\0';
				ImGui::OpenPopup("##new_world_popup");
			}
			draw_new_world_popup();
			ImGui::Separator();
		}

		// Filter bar
		ImGui::SetNextItemWidth(-1.f);
		ImGui::InputTextWithHint("##filter", "Filter by name...", m_filter, sizeof(m_filter));

		// New root node button
		ImGui::BeginDisabled(!m_world_desc);
		if (ImGui::Button("+ New Entity"))
			ImGui::OpenPopup("##new_entity_type");
		ImGui::EndDisabled();

		if (ImGui::BeginPopup("##new_entity_type")) {
			struct { const char* label; std::string_view type; } types[] = {
				{ "Empty",             "Empty"                            },
				{ "Camera",            scn::camera_desc::__type           },
				{ "Directional Light", scn::directional_light_desc::__type},
				{ "Skybox",            scn::skybox_desc::__type           },
				{ "Prefab",            scn::prefab_desc::__type           },
			};
			for (const auto& t : types) {
				if (ImGui::MenuItem(t.label)) {
					if (m_on_create_node) m_on_create_node(std::string(t.type));
				}
			}
			ImGui::EndPopup();
		}

		ImGui::Separator();

		if (!m_world_desc)
			return;

		std::string lower_filter = m_filter;
		std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);

		// Rebuild flat visible order for shift-select
		m_flat_visible_nodes.clear();
		collect_flat_order(m_world_desc->get_root().children, lower_filter);

		// Delete key shortcut
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
			ImGui::IsKeyPressed(ImGuiKey_Delete) &&
			((m_selected_node && !m_selected_readonly) || m_multi_selected.size() > 1))
		{
			delete_selected();
			return;
		}

		auto& children = m_world_desc->get_root().children;
		int flat_index = 0;
		for (int i = 0; i < (int)children.size(); ++i)
			draw_node(children[i], lower_filter, flat_index);

		// ── drop on empty area → move to root ────────────────────────────
		ImVec2 avail = ImGui::GetContentRegionAvail();
		if (avail.y > 0.f) {
			ImGui::InvisibleButton("##root_drop_area", ImVec2(-1.f, avail.y));
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
					uint64_t dragged_id = 0;
					std::memcpy(&dragged_id, payload->Data, sizeof(dragged_id));
					// parent id 0 = root
					if (m_on_reparent_node) m_on_reparent_node(dragged_id, 0, -1);
				}
				ImGui::EndDragDropTarget();
			}
		}
	}

	void scene_hierarchy_panel::draw_node(scn::prefab_desc::prefab_node& node, const std::string& lower_filter, int& flat_index)
	{
		// ── filter ──────────────────────────────────────────────────────────
		const bool filter_active = !lower_filter.empty();
		if (filter_active && !node_matches_filter(node, lower_filter))
			return;

		const int my_flat_index = flat_index++;

		const char* prefix = get_type_prefix(node);

		// Build ImGui IDs using format_to_n into stack buffer (no heap allocation)
		char id_buf[512];
		auto id_result = std::format_to_n(id_buf, sizeof(id_buf) - 1, "{0}{1}##{2:x}", prefix, node.name, node.node_id);
		*id_result.out = '\0';

		// ── inline rename ────────────────────────────────────────────────────
		if (m_rename_node == &node) {
			char rename_id[512];
			auto rn_result = std::format_to_n(rename_id, sizeof(rename_id) - 1, "##rename##{:x}", node.node_id);
			*rn_result.out = '\0';
			ImGui::SetKeyboardFocusHere();
			if (ImGui::InputText(rename_id, m_rename_buf, sizeof(m_rename_buf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
			{
				if (m_rename_buf[0] != '\0' && node.name != m_rename_buf) {
					std::string old_name = node.name;
					node.name = m_rename_buf;
					if (m_on_rename_node) m_on_rename_node(old_name, node.name);
				}
				m_rename_node = nullptr;
			}
			if (ImGui::IsItemDeactivated())
				m_rename_node = nullptr;
			return;
		}

		// ── tree node ────────────────────────────────────────────────────────
		const scn::prefab_desc* ref_prefab = get_referenced_prefab(node);
		const bool has_children = !node.children.empty() || ref_prefab;
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
		if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf;

		const bool is_primary   = (&node == m_selected_node);
		const bool is_in_multi  = m_multi_selected.count(&node) > 0;
		if (is_primary || is_in_multi) flags |= ImGuiTreeNodeFlags_Selected;

		// Auto-expand parents when filter is active so matching children are visible
		if (filter_active && has_children)
			ImGui::SetNextItemOpen(true, ImGuiCond_Always);

		const bool opened = ImGui::TreeNodeEx(id_buf, flags);

		// ── selection ───────────────────────────────────────────────────────
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
			const bool ctrl_held  = ImGui::GetIO().KeyCtrl;
			const bool shift_held = ImGui::GetIO().KeyShift;

			if (ctrl_held) {
				// Ctrl+Click: toggle node in multi-selection
				if (m_multi_selected.count(&node)) {
					m_multi_selected.erase(&node);
					// If we removed the primary, pick another or clear
					if (&node == m_selected_node) {
						m_selected_node = m_multi_selected.empty() ? nullptr : *m_multi_selected.begin();
						notify_selected();
					}
				} else {
					// Add current primary to multi_selected first
					if (m_selected_node && m_multi_selected.empty())
						m_multi_selected.insert(m_selected_node);
					m_multi_selected.insert(&node);
					m_selected_node = &node;
					m_selected_readonly = false;
					notify_selected();
				}
				m_last_clicked_flat_idx = my_flat_index;
			} else if (shift_held && m_last_clicked_flat_idx >= 0) {
				// Shift+Click: select range in flat visible order
				m_multi_selected.clear();
				const int from = std::min(m_last_clicked_flat_idx, my_flat_index);
				const int to   = std::max(m_last_clicked_flat_idx, my_flat_index);
				for (int i = from; i <= to && i < (int)m_flat_visible_nodes.size(); ++i)
					m_multi_selected.insert(m_flat_visible_nodes[i]);
				m_selected_node = &node;
				m_selected_readonly = false;
				notify_selected();
			} else {
				// Plain click: single selection
				m_multi_selected.clear();
				m_selected_node = &node;
				m_selected_readonly = false;
				m_last_clicked_flat_idx = my_flat_index;
				notify_selected();
			}
		}

		// ── drag source ─────────────────────────────────────────────────────
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
			ImGui::SetDragDropPayload("HIERARCHY_NODE", &node.node_id, sizeof(node.node_id));
			ImGui::TextUnformatted(node.name.c_str());
			ImGui::EndDragDropSource();
		}

		// ── drop target: position-based (top edge = insert before, middle = make child, bottom edge = insert after)
		if (ImGui::BeginDragDropTarget()) {
			// Draw visual hint based on mouse position
			ImVec2 item_min = ImGui::GetItemRectMin();
			ImVec2 item_max = ImGui::GetItemRectMax();
			float item_h = item_max.y - item_min.y;
			float mouse_y = ImGui::GetMousePos().y - item_min.y;
			float fraction = (item_h > 0.f) ? (mouse_y / item_h) : 0.5f;

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			ImU32 line_col = IM_COL32(80, 160, 255, 200);
			float line_thickness = 2.0f;

			if (fraction < 0.25f) {
				// Line above
				draw_list->AddLine(ImVec2(item_min.x, item_min.y), ImVec2(item_max.x, item_min.y), line_col, line_thickness);
			} else if (fraction > 0.75f) {
				// Line below
				draw_list->AddLine(ImVec2(item_min.x, item_max.y), ImVec2(item_max.x, item_max.y), line_col, line_thickness);
			} else {
				// Highlight rect (make child)
				draw_list->AddRect(item_min, item_max, line_col, 0.f, 0, line_thickness);
			}

			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
				uint64_t dragged_id = 0;
				std::memcpy(&dragged_id, payload->Data, sizeof(dragged_id));
				if (dragged_id != node.node_id) {
					if (fraction < 0.25f) {
						if (m_on_reparent_node) m_on_reparent_node(dragged_id, node.node_id, 0);
					} else if (fraction > 0.75f) {
						if (m_on_reparent_node) m_on_reparent_node(dragged_id, node.node_id, 1);
					} else {
						if (m_on_reparent_node) m_on_reparent_node(dragged_id, node.node_id, -1);
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		// ── double-click to rename ───────────────────────────────────────────
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			m_rename_node = &node;
			auto cp = std::format_to_n(m_rename_buf, sizeof(m_rename_buf) - 1, "{}", node.name);
			*cp.out = '\0';
		}

		// ── context menu ─────────────────────────────────────────────────────
		if (ImGui::BeginPopupContextItem()) {
			const bool multi_mode = m_multi_selected.size() > 1;

			if (!multi_mode) {
				if (ImGui::MenuItem("Rename")) {
					m_rename_node = &node;
					auto cp = std::format_to_n(m_rename_buf, sizeof(m_rename_buf) - 1, "{}", node.name);
					*cp.out = '\0';
				}
			}

			if (ImGui::MenuItem(multi_mode ? "Duplicate Selection" : "Duplicate")) {
				ImGui::EndPopup();
				if (opened) ImGui::TreePop();
				if (multi_mode) {
					duplicate_selected();
				} else {
					const uint64_t id = node.node_id;
					m_multi_selected.clear();
					m_selected_node = nullptr;
					if (m_on_duplicate_node) m_on_duplicate_node(id);
				}
				return;
			}

			ImGui::Separator();

			if (ImGui::MenuItem(multi_mode ? "Delete Selection" : "Delete")) {
				ImGui::EndPopup();
				if (opened) ImGui::TreePop();
				if (multi_mode) {
					delete_selected();
				} else {
					const uint64_t id = node.node_id;
					m_selected_node = nullptr;
					m_multi_selected.clear();
					if (m_on_delete_node) m_on_delete_node(id);
				}
				return;
			}

			ImGui::EndPopup();
		}

		// ── children ─────────────────────────────────────────────────────────
		if (opened) {
			for (auto& child : node.children)
				draw_node(child, lower_filter, flat_index);
			// Show the instantiated nodes as read-only (their edits become override records)
			if (ref_prefab) {
				std::vector<uint64_t> target;
				draw_instance_children(node, target);
			}
			ImGui::TreePop();
		}
	}
}
