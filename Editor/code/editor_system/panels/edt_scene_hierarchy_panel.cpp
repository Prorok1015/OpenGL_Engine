#include "edt_scene_hierarchy_panel.h"
#include "scn_camera_desc.h"
#include "scn_directional_light_desc.h"
#include "scn_mesh_node_desc.h"
#include "scn_skybox_desc.h"
#include "scn_bone_desc.h"
#include <imgui.h>
#include <string>
#include <algorithm>
#include <cctype>
#include <format>

namespace edt
{
	scene_hierarchy_panel::scene_hierarchy_panel()
		: panel_base("scene_hierarchy", "Scene Hierarchy")
	{
	}

	void scene_hierarchy_panel::set_world_desc(scn::world_desc* desc)
	{
		m_world_desc    = desc;
		m_selected_node = nullptr;
		m_rename_node   = nullptr;
	}

	void scene_hierarchy_panel::set_on_node_selected(std::function<void(scn::prefab_desc::prefab_node*)> cb)
	{
		m_on_node_selected = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_delete_node(std::function<void(const std::string&)> cb)
	{
		m_on_delete_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_create_node(std::function<void(const std::string&)> cb)
	{
		m_on_create_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_rename_node(std::function<void(const std::string&, const std::string&)> cb)
	{
		m_on_rename_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_duplicate_node(std::function<void(const std::string&)> cb)
	{
		m_on_duplicate_node = std::move(cb);
	}

	void scene_hierarchy_panel::set_on_reparent_node(std::function<void(const std::string&, const std::string&, int)> cb)
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

	void scene_hierarchy_panel::draw_prefab_children_readonly(const scn::prefab_desc::prefab_node& node)
	{
		for (const auto& child : node.children) {
			const bool has_sub = !child.children.empty();
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
			if (!has_sub) flags |= ImGuiTreeNodeFlags_Leaf;

			std::string prefix;
			if (child.components.count(std::string(scn::mesh_node_desc::__type)))   prefix = "[M] ";
			else if (child.components.count(std::string(scn::bone_desc::__type)))   prefix = "[B] ";
			else if (child.components.count(std::string(scn::camera_desc::__type))) prefix = "[C] ";

			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
			std::string id = prefix + child.name + "##prefab_" + child.name;
			bool opened = ImGui::TreeNodeEx(id.c_str(), flags);
			ImGui::PopStyleColor();

			if (opened) {
				draw_prefab_children_readonly(child);
				ImGui::TreePop();
			}
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
						m_rename_node   = nullptr;
						if (m_on_world_changed) m_on_world_changed(i);
					}
					ImGui::EndTabItem();
				}
			}

			if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
				m_new_world_buf[0] = '\0';
				ImGui::OpenPopup("##new_world_popup");
			}

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

			ImGui::EndTabBar();
		}

		m_force_tab_switch = false;
	}

	void scene_hierarchy_panel::on_render()
	{
		if (!m_world_names.empty()) {
			draw_world_tabs();
			ImGui::Separator();
		}

		// Filter bar
		ImGui::SetNextItemWidth(-1.f);
		ImGui::InputTextWithHint("##filter", "Filter by name...", m_filter, sizeof(m_filter));

		// New root node button
		if (ImGui::Button("+ New Entity"))
			ImGui::OpenPopup("##new_entity_type");

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

		auto& children = m_world_desc->get_root().children;
		for (int i = 0; i < children.size(); ++i)
			draw_node(children[i], lower_filter);

		// ── drop on empty area → move to root ────────────────────────────
		ImVec2 avail = ImGui::GetContentRegionAvail();
		if (avail.y > 0.f) {
			ImGui::InvisibleButton("##root_drop_area", ImVec2(-1.f, avail.y));
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_NODE")) {
					std::string dragged_name(static_cast<const char*>(payload->Data));
					// empty parent name = root
					if (m_on_reparent_node) m_on_reparent_node(dragged_name, "", -1);
				}
				ImGui::EndDragDropTarget();
			}
		}
	}

	void scene_hierarchy_panel::draw_node(scn::prefab_desc::prefab_node& node, const std::string& lower_filter)
	{
		// ── filter ──────────────────────────────────────────────────────────
		const bool filter_active = !lower_filter.empty();
		if (filter_active && !node_matches_filter(node, lower_filter))
			return;

		const char* prefix = get_type_prefix(node);

		// Build ImGui IDs using format_to_n into stack buffer (no heap allocation)
		char id_buf[512];
		auto id_result = std::format_to_n(id_buf, sizeof(id_buf) - 1, "{}{}##{}", prefix, node.name, node.name);
		*id_result.out = '\0';

		// ── inline rename ────────────────────────────────────────────────────
		if (m_rename_node == &node) {
			char rename_id[512];
			auto rn_result = std::format_to_n(rename_id, sizeof(rename_id) - 1, "##rename##{}", node.name);
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
		const bool has_children = !node.children.empty() || (ref_prefab && !ref_prefab->get_root().children.empty());
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
		if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf;
		if (&node == m_selected_node) flags |= ImGuiTreeNodeFlags_Selected;
		// Auto-expand parents when filter is active so matching children are visible
		if (filter_active && has_children)
			ImGui::SetNextItemOpen(true, ImGuiCond_Always);

		const bool opened = ImGui::TreeNodeEx(id_buf, flags);

		// ── selection ───────────────────────────────────────────────────────
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
			m_selected_node = &node;
			if (m_on_node_selected) m_on_node_selected(&node);
		}

		// ── drag source ─────────────────────────────────────────────────────
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
			ImGui::SetDragDropPayload("HIERARCHY_NODE", node.name.c_str(), node.name.size() + 1);
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
				std::string dragged_name(static_cast<const char*>(payload->Data));
				if (dragged_name != node.name) {
					if (fraction < 0.25f) {
						if (m_on_reparent_node) m_on_reparent_node(dragged_name, node.name, 0);
					} else if (fraction > 0.75f) {
						if (m_on_reparent_node) m_on_reparent_node(dragged_name, node.name, 1);
					} else {
						if (m_on_reparent_node) m_on_reparent_node(dragged_name, node.name, -1);
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
			if (ImGui::MenuItem("Rename")) {
				m_rename_node = &node;
				strncpy(m_rename_buf, node.name.c_str(), sizeof(m_rename_buf) - 1);
				m_rename_buf[sizeof(m_rename_buf) - 1] = '\0';
			}
			if (ImGui::MenuItem("Duplicate")) {
				std::string name = node.name;
				ImGui::EndPopup();
				if (opened) ImGui::TreePop();
				if (m_on_duplicate_node) m_on_duplicate_node(name);
				return;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Delete")) {
				std::string name = node.name;
				ImGui::EndPopup();
				if (opened) ImGui::TreePop();
				if (m_on_delete_node) m_on_delete_node(name);
				return;
			}
			ImGui::EndPopup();
		}

		// ── children ─────────────────────────────────────────────────────────
		if (opened) {
			for (auto& child : node.children)
				draw_node(child, lower_filter);
			// Show referenced prefab's children as read-only
			if (ref_prefab)
				draw_prefab_children_readonly(ref_prefab->get_root());
			ImGui::TreePop();
		}
	}
}
