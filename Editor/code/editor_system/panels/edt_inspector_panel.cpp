#include "edt_inspector_panel.h"
#include "eng_transform_3d.hpp"
#include "scn_camera_component.hpp"

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

	void inspector_panel::on_render()
	{
		if (!m_registry || m_selected == entt::null) {
			ImGui::TextDisabled("No entity selected");
			return;
		}

		ImGui::Text("Entity %u", (uint32_t)entt::to_integral(m_selected));
		ImGui::Separator();

		draw_transform_component();
		draw_camera_component();

		ImGui::Separator();
		draw_add_component_popup();
	}

	void inspector_panel::draw_transform_component()
	{
		if (!m_registry->all_of<eng::transform3d>(m_selected)) {
			return;
		}

		if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
			auto& t = m_registry->get<eng::transform3d>(m_selected);

			glm::vec3 position = t.get_pos();
			if (ImGui::DragFloat3("Position", &position.x, 0.1f)) {
				t.set_pos(position);
			}

			glm::vec3 rotation_deg = t.get_angles_degrees();
			if (ImGui::DragFloat3("Rotation", &rotation_deg.x, 0.5f)) {
				t.set_euler_angles(glm::radians(rotation_deg));
			}

			glm::vec3 scale = t.get_scale();
			if (ImGui::DragFloat3("Scale", &scale.x, 0.01f, 0.001f, 100.f)) {
				t.set_scale(scale);
			}
		}
	}

	void inspector_panel::draw_camera_component()
	{
		if (!m_registry->all_of<scn::camera_component>(m_selected)) {
			return;
		}

		if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
			auto& cam = m_registry->get<scn::camera_component>(m_selected);

			ImGui::DragFloat("FOV", &cam.fov, 0.5f, 1.0f, 179.0f);
			ImGui::DragFloat("Near", &cam.near_distance, 0.001f, 0.0001f, 10.0f, "%.4f");
			ImGui::DragFloat("Far", &cam.far_distance, 1.0f, 1.0f, 100000.0f);
		}
	}

	void inspector_panel::draw_add_component_popup()
	{
		if (ImGui::Button("+ Add Component")) {
			ImGui::OpenPopup("add_comp");
		}

		if (ImGui::BeginPopup("add_comp")) {
			if (ImGui::MenuItem("Transform")) {
				if (!m_registry->all_of<eng::transform3d>(m_selected)) {
					m_registry->emplace<eng::transform3d>(m_selected);
				}
			}
			if (ImGui::MenuItem("Camera")) {
				if (!m_registry->all_of<scn::camera_component>(m_selected)) {
					m_registry->emplace<scn::camera_component>(m_selected);
				}
			}
			ImGui::EndPopup();
		}
	}
}
