#include "edt_scene_hierarchy_panel.h"

#include <string>

namespace edt
{
	scene_hierarchy_panel::scene_hierarchy_panel()
		: panel_base("scene_hierarchy", "Scene Hierarchy")
	{
	}

	void scene_hierarchy_panel::set_registry(std::shared_ptr<entt::registry> registry)
	{
		m_registry = std::move(registry);
	}

	void scene_hierarchy_panel::set_on_entity_selected(std::function<void(ecs::entity)> cb)
	{
		m_on_selected = std::move(cb);
	}

	void scene_hierarchy_panel::on_render()
	{
		ImGui::InputText("##filter", m_filter, sizeof(m_filter));
		ImGui::SameLine();
		ImGui::Text("Filter");

		if (!m_registry) {
			return;
		}

		for (auto ent : m_registry->storage<entt::entity>())
		{
			if (m_registry->valid(ent))
				draw_entity_node(ent);
		}
	}

	void scene_hierarchy_panel::draw_entity_node(ecs::entity ent)
	{
		std::string label = "Entity " + std::to_string((uint32_t)entt::to_integral(ent));

		if (m_filter[0] != 0 && label.find(m_filter) == std::string::npos) {
			return;
		}

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanFullWidth;
		if (ent == m_selected) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		bool opened = ImGui::TreeNodeEx(label.c_str(), flags);

		if (ImGui::IsItemClicked()) {
			m_selected = ent;
			if (m_on_selected) {
				m_on_selected(ent);
			}
		}

		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Delete")) {
				m_registry->destroy(ent);
			}
			ImGui::EndPopup();
		}

		if (opened) {
			ImGui::TreePop();
		}
	}
}
