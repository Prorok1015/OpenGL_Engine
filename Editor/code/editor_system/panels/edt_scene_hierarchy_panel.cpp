#include "edt_scene_hierarchy_panel.h"
#include "scn_model.h"
#include "scn_camera_component.hpp"
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

	void scene_hierarchy_panel::set_registry(std::shared_ptr<entt::registry> registry)
	{
		m_registry = std::move(registry);
		m_selected      = entt::null;
		m_rename_entity = entt::null;
	}

	void scene_hierarchy_panel::set_on_entity_selected(std::function<void(ecs::entity)> cb)
	{
		m_on_selected = std::move(cb);
	}

	// ─── helpers ──────────────────────────────────────────────────────────────

	const char* scene_hierarchy_panel::get_type_prefix(ecs::entity ent) const
	{
		if (m_registry->all_of<scn::scene_anchor_component>(ent)) return "[A] ";
		if (m_registry->all_of<scn::camera_component>(ent))       return "[C] ";
		if (m_registry->all_of<scn::directional_light>(ent))      return "[L] ";
		if (m_registry->all_of<scn::mesh_component>(ent))         return "[M] ";
		if (m_registry->all_of<scn::bone_component>(ent))         return "[B] ";
		return "";
	}

	std::string scene_hierarchy_panel::get_display_name(ecs::entity ent) const
	{
		std::string prefix = get_type_prefix(ent);
		if (m_registry->all_of<scn::name_component>(ent)) {
			const auto& n = m_registry->get<scn::name_component>(ent);
			if (!n.name.empty())
				return prefix + n.name;
		}
		return prefix + "Entity #" + std::to_string(static_cast<uint32_t>(entt::to_integral(ent)));
	}

	void scene_hierarchy_panel::destroy_entity(ecs::entity ent)
	{
		// Remove from parent's children list first
		if (m_registry->all_of<scn::parent_component>(ent)) {
			entt::entity parent = m_registry->get<scn::parent_component>(ent).parent;
			if (m_registry->valid(parent) && m_registry->all_of<scn::children_component>(parent)) {
				auto& ch = m_registry->get<scn::children_component>(parent);
				ch.children.erase(
					std::remove(ch.children.begin(), ch.children.end(), ent),
					ch.children.end());
			}
		}

		// Recursively destroy children
		if (m_registry->all_of<scn::children_component>(ent)) {
			// Copy the list — it will be modified by recursive calls
			auto children = m_registry->get<scn::children_component>(ent).children;
			for (entt::entity child : children)
				if (m_registry->valid(child))
					destroy_entity(child);
		}

		if (m_registry->valid(ent))
			m_registry->destroy(ent);

		if (m_selected == ent) {
			m_selected = entt::null;
			if (m_on_selected) m_on_selected(entt::null);
		}
	}

	// ─── rendering ────────────────────────────────────────────────────────────

	void scene_hierarchy_panel::on_render()
	{
		// Filter bar
		ImGui::SetNextItemWidth(-1.f);
		ImGui::InputTextWithHint("##filter", "Filter by name...", m_filter, sizeof(m_filter));

		// New root entity button
		if (ImGui::Button("+ New Entity")) {
			if (m_registry) {
				entt::entity e = m_registry->create();
				m_registry->emplace<scn::name_component>(e, "New Entity");
				m_registry->emplace<scn::local_transform>(e);
				m_selected = e;
				if (m_on_selected) m_on_selected(e);
			}
		}

		ImGui::Separator();

		if (!m_registry)
			return;

		// Draw only root entities (no parent) — recurse into children
		for (auto ent : m_registry->storage<entt::entity>()) {
			if (!m_registry->valid(ent))
				continue;
			if (m_registry->all_of<scn::parent_component>(ent))
				continue; // not a root
			draw_entity_node(ent);
		}
	}

	void scene_hierarchy_panel::draw_entity_node(ecs::entity ent)
	{
		const std::string label = get_display_name(ent);
		const std::string uid   = "##" + std::to_string(static_cast<uint32_t>(entt::to_integral(ent)));

		// ── filter ──────────────────────────────────────────────────────────
		if (m_filter[0] != '\0') {
			// Case-insensitive substring search
			std::string lower_label = label;
			std::string lower_filter = m_filter;
			std::transform(lower_label.begin(), lower_label.end(), lower_label.begin(), ::tolower);
			std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);
			if (lower_label.find(lower_filter) == std::string::npos) {
				// Still recurse into children — a child might match
				if (m_registry->all_of<scn::children_component>(ent))
					for (entt::entity child : m_registry->get<scn::children_component>(ent).children)
						if (m_registry->valid(child)) draw_entity_node(child);
				return;
			}
		}

		// ── inline rename mode ───────────────────────────────────────────────
		if (m_rename_entity == ent) {
			ImGui::SetKeyboardFocusHere();
			if (ImGui::InputText(("##rename" + uid).c_str(), m_rename_buf, sizeof(m_rename_buf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
			{
				if (!m_registry->all_of<scn::name_component>(ent))
					m_registry->emplace<scn::name_component>(ent);
				m_registry->get<scn::name_component>(ent).name = m_rename_buf;
				m_rename_entity = entt::null;
			}
			if (ImGui::IsItemDeactivated())
				m_rename_entity = entt::null;
			return;
		}

		// ── tree node ────────────────────────────────────────────────────────
		const bool has_children = m_registry->all_of<scn::children_component>(ent) &&
			!m_registry->get<scn::children_component>(ent).children.empty();

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
		if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf;
		if (ent == m_selected) flags |= ImGuiTreeNodeFlags_Selected;

		const std::string node_id = label + uid;
		const bool opened = ImGui::TreeNodeEx(node_id.c_str(), flags);

		// ── selection ───────────────────────────────────────────────────────
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
			m_selected = ent;
			if (m_on_selected) m_on_selected(ent);
		}

		// ── double-click to rename ───────────────────────────────────────────
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			m_rename_entity = ent;
			const std::string current = m_registry->all_of<scn::name_component>(ent)
				? m_registry->get<scn::name_component>(ent).name
				: std::string{};
			strncpy(m_rename_buf, current.c_str(), sizeof(m_rename_buf) - 1);
			m_rename_buf[sizeof(m_rename_buf) - 1] = '\0';
		}

		// ── context menu ─────────────────────────────────────────────────────
		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Rename")) {
				m_rename_entity = ent;
				const std::string current = m_registry->all_of<scn::name_component>(ent)
					? m_registry->get<scn::name_component>(ent).name
					: std::string{};
				strncpy(m_rename_buf, current.c_str(), sizeof(m_rename_buf) - 1);
				m_rename_buf[sizeof(m_rename_buf) - 1] = '\0';
			}

			if (ImGui::MenuItem("Add Child Entity")) {
				entt::entity child = m_registry->create();
				m_registry->emplace<scn::name_component>(child, "New Entity");
				m_registry->emplace<scn::local_transform>(child);
				m_registry->emplace<scn::parent_component>(child, ent);
				if (m_registry->all_of<scn::children_component>(ent)) {
					m_registry->get<scn::children_component>(ent).children.push_back(child);
				} else {
					m_registry->emplace<scn::children_component>(ent,
						std::vector<entt::entity>{ child });
				}
				m_selected = child;
				if (m_on_selected) m_on_selected(child);
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Delete")) {
				ImGui::EndPopup();
				if (opened) ImGui::TreePop();
				destroy_entity(ent);
				return;
			}
			ImGui::EndPopup();
		}

		// ── children ─────────────────────────────────────────────────────────
		if (opened) {
			if (has_children)
				for (entt::entity child : m_registry->get<scn::children_component>(ent).children)
					if (m_registry->valid(child))
						draw_entity_node(child);
			ImGui::TreePop();
		}
	}
}
