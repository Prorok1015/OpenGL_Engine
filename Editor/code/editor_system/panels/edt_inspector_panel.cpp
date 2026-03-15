#include "edt_inspector_panel.h"
#include "scn_model.h"
#include <imgui.h>

namespace edt
{
	inspector_panel::inspector_panel()
		: panel_base("inspector", "Inspector")
	{
	}

	void inspector_panel::set_registry(std::shared_ptr<entt::registry> registry)
	{
		m_registry = std::move(registry);
	}

	void inspector_panel::add_component_renderer(component_draw_fn fn)
	{
		m_renderers.push_back(std::move(fn));
	}

	void inspector_panel::add_component_creator(std::string name, std::function<void(entt::registry&, entt::entity)> fn)
	{
		m_creators.push_back({std::move(name), std::move(fn)});
	}

	void inspector_panel::on_render()
	{
		if (!m_registry || m_selected == entt::null || !m_registry->valid(m_selected)) {
			ImGui::TextDisabled("No entity selected");
			return;
		}

		// Header: show entity name if available, otherwise fall back to ID
		if (m_registry->all_of<scn::name_component>(m_selected)) {
			const auto& n = m_registry->get<scn::name_component>(m_selected);
			ImGui::TextUnformatted(n.name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("(#%u)", static_cast<uint32_t>(entt::to_integral(m_selected)));
		} else {
			ImGui::Text("Entity #%u", static_cast<uint32_t>(entt::to_integral(m_selected)));
		}
		ImGui::Separator();

		for (auto& fn : m_renderers)
			fn(*m_registry, m_selected);

		ImGui::Separator();
		draw_add_component_popup();
	}

	void inspector_panel::draw_add_component_popup()
	{
		if (ImGui::Button("+ Add Component"))
			ImGui::OpenPopup("add_comp");

		if (ImGui::BeginPopup("add_comp")) {
			for (auto& creator : m_creators) {
				if (ImGui::MenuItem(creator.name.c_str()))
					creator.create_fn(*m_registry, m_selected);
			}
			ImGui::EndPopup();
		}
	}
}
