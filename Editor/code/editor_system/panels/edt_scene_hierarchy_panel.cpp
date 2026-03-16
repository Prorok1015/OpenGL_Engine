#include "edt_scene_hierarchy_panel.h"
#include <imgui.h>
#include <string>
#include <algorithm>
#include <cctype>

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
		if (node.components.count("camera_desc"))             return "[C] ";
		if (node.components.count("directional_light_desc"))  return "[L] ";
		if (node.components.count("skin_prototype_desc"))     return "[M] ";
		if (node.components.count("skinning_prototype_desc")) return "[M] ";
		if (node.components.count("skybox_desc"))             return "[S] ";
		return "";
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
			struct { const char* label; const char* type; } types[] = {
				{ "Empty",             "Empty"                   },
				{ "Camera",            "camera_desc"             },
				{ "Directional Light", "directional_light_desc"  },
				{ "Skybox",            "skybox_desc"             },
				{ "Skinned Model",     "skinning_prototype_desc" },
			};
			for (const auto& t : types) {
				if (ImGui::MenuItem(t.label)) {
					if (m_on_create_node) m_on_create_node(t.type);
				}
			}
			ImGui::EndPopup();
		}

		ImGui::Separator();

		if (!m_world_desc)
			return;

		std::string lower_filter = m_filter;
		std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);

		for (auto& child : m_world_desc->get_root().children)
			draw_node(child, lower_filter);
	}

	void scene_hierarchy_panel::draw_node(scn::prefab_desc::prefab_node& node, const std::string& lower_filter)
	{
		// ── filter ──────────────────────────────────────────────────────────
		const bool filter_active = !lower_filter.empty();
		if (filter_active && !node_matches_filter(node, lower_filter))
			return;

		const std::string prefix  = get_type_prefix(node);
		const std::string label   = prefix + node.name;
		const std::string uid     = "##" + node.name;

		// ── inline rename ────────────────────────────────────────────────────
		if (m_rename_node == &node) {
			ImGui::SetKeyboardFocusHere();
			if (ImGui::InputText(("##rename" + uid).c_str(), m_rename_buf, sizeof(m_rename_buf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
			{
				if (m_rename_buf[0] != '\0' && m_rename_buf != node.name) {
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
		const bool has_children = !node.children.empty();
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
		if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf;
		if (&node == m_selected_node) flags |= ImGuiTreeNodeFlags_Selected;
		// Auto-expand parents when filter is active so matching children are visible
		if (filter_active && has_children)
			ImGui::SetNextItemOpen(true, ImGuiCond_Always);

		const std::string node_id = label + uid;
		const bool opened = ImGui::TreeNodeEx(node_id.c_str(), flags);

		// ── selection ───────────────────────────────────────────────────────
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
			m_selected_node = &node;
			if (m_on_node_selected) m_on_node_selected(&node);
		}

		// ── double-click to rename ───────────────────────────────────────────
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			m_rename_node = &node;
			strncpy(m_rename_buf, node.name.c_str(), sizeof(m_rename_buf) - 1);
			m_rename_buf[sizeof(m_rename_buf) - 1] = '\0';
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
			ImGui::TreePop();
		}
	}
}
