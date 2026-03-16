#include "edt_inspector_panel.h"
#include <imgui.h>
#include <boost/json.hpp>
#include <glm/glm.hpp>
#include <string>

namespace edt
{
	inspector_panel::inspector_panel()
		: panel_base("inspector", "Inspector")
	{
	}

	void inspector_panel::set_selected_node(scn::prefab_desc::prefab_node* node)
	{
		m_selected_node = node;
	}

	void inspector_panel::set_on_node_changed(std::function<void()> cb)
	{
		m_on_node_changed = std::move(cb);
	}

	void inspector_panel::add_desc_renderer(const std::string& type_name, desc_comp_renderer renderer)
	{
		m_desc_renderers[type_name] = std::move(renderer);
	}

	void inspector_panel::on_render()
	{
		if (!m_selected_node) {
			ImGui::TextDisabled("No node selected");
			return;
		}

		auto& node = *m_selected_node;
		bool changed = false;

		// Header
		ImGui::TextUnformatted(node.name.c_str());
		ImGui::Separator();

		// Transform
		if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::DragFloat3("Position", &node.position.x, 0.01f)) changed = true;
			if (ImGui::DragFloat3("Rotation", &node.rotation.x, 0.5f))  changed = true;
			if (ImGui::DragFloat3("Scale",    &node.scale.x,    0.01f, 0.001f, 100.f)) changed = true;
		}

		ImGui::Separator();

		// Components
		for (auto& [key, comp] : node.components) {
			if (ImGui::CollapsingHeader(comp.type_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
				auto it = m_desc_renderers.find(comp.type_name);
				if (it != m_desc_renderers.end()) {
					changed |= it->second(comp);
				} else {
					render_generic_json(comp.overrides);
				}
			}
		}

		ImGui::Separator();
		draw_add_component_popup();

		if (changed && m_on_node_changed)
			m_on_node_changed();
	}

	void inspector_panel::render_generic_json(boost::json::object& obj)
	{
		for (auto& [key, val] : obj) {
			std::string k(key);
			if (k.starts_with("__")) continue; // skip meta fields

			if (val.is_double()) {
				float f = static_cast<float>(val.as_double());
				if (ImGui::DragFloat(k.c_str(), &f, 0.01f))
					val = static_cast<double>(f);
			} else if (val.is_int64()) {
				int i = static_cast<int>(val.as_int64());
				if (ImGui::DragInt(k.c_str(), &i))
					val = static_cast<int64_t>(i);
			} else if (val.is_string()) {
				char buf[512];
				std::string s(val.as_string());
				strncpy(buf, s.c_str(), sizeof(buf) - 1);
				buf[sizeof(buf) - 1] = '\0';
				if (ImGui::InputText(k.c_str(), buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
					val = std::string(buf);
			} else if (val.is_array() && val.as_array().size() == 3) {
				auto& arr = val.as_array();
				glm::vec3 v{
					static_cast<float>(arr[0].is_double() ? arr[0].as_double() : 0.0),
					static_cast<float>(arr[1].is_double() ? arr[1].as_double() : 0.0),
					static_cast<float>(arr[2].is_double() ? arr[2].as_double() : 0.0)
				};
				if (ImGui::DragFloat3(k.c_str(), &v.x, 0.01f)) {
					arr[0] = static_cast<double>(v.x);
					arr[1] = static_cast<double>(v.y);
					arr[2] = static_cast<double>(v.z);
				}
			} else if (val.is_bool()) {
				bool b = val.as_bool();
				if (ImGui::Checkbox(k.c_str(), &b))
					val = b;
			} else {
				ImGui::TextDisabled("%s: (complex)", k.c_str());
			}
		}
	}

	void inspector_panel::draw_add_component_popup()
	{
		if (!m_selected_node) return;

		if (ImGui::Button("+ Add Component"))
			ImGui::OpenPopup("add_comp_desc");

		if (ImGui::BeginPopup("add_comp_desc")) {
			struct { const char* label; const char* type; } known[] = {
				{ "Camera",            "camera_desc"            },
				{ "Directional Light", "directional_light_desc" },
				{ "Skybox",            "skybox_desc"            },
			};
			bool changed = false;
			for (const auto& entry : known) {
				if (!m_selected_node->components.count(entry.type)) {
					if (ImGui::MenuItem(entry.label)) {
						scn::prefab_desc::prefab_comp_node comp;
						comp.type_name = entry.type;
						m_selected_node->components[entry.type] = std::move(comp);
						changed = true;
					}
				}
			}
			ImGui::EndPopup();
			if (changed && m_on_node_changed)
				m_on_node_changed();
		}
	}
}
