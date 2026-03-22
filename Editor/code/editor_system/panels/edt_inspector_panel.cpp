#include "edt_inspector_panel.h"
#include "edt_component_ui_registry.h"
#include "desc_system.h"
#include <imgui.h>
#include <boost/json.hpp>
#include <glm/glm.hpp>
#include <string>
#include <format>

namespace edt
{
	inspector_panel::inspector_panel()
		: panel_base("inspector", "Inspector")
	{
	}

	void inspector_panel::set_selected_node(scn::prefab_desc::prefab_node* node, bool readonly)
	{
		m_selected_node = node;
		m_readonly = readonly;
		rebuild_parent_cache();
	}

	void inspector_panel::rebuild_parent_cache()
	{
		m_parent_cache.clear();
		if (!m_selected_node) return;

		for (auto& [key, comp] : m_selected_node->components) {
			if (comp.parent_desc.is_valid() && comp.parent_desc.is_ready() && comp.overrides.empty()) {
				boost::json::object data;
				comp.parent_desc->serialize(data);
				m_parent_cache[key] = std::move(data);
			}
		}
	}

	void inspector_panel::set_on_node_changed(std::function<void()> cb)
	{
		m_on_node_changed = std::move(cb);
	}

	void inspector_panel::set_component_ui_registry(component_ui_registry* registry)
	{
		m_ui_registry = registry;
	}

	void inspector_panel::set_desc_system(desc::desc_system* desc_system)
	{
		m_desc_system = desc_system;
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
		if (m_readonly) {
			ImGui::TextDisabled("(read-only) %s", node.name.c_str());
		} else {
			ImGui::TextUnformatted(node.name.c_str());
		}
		ImGui::Separator();

		if (m_readonly) ImGui::BeginDisabled();

		// Transform
		if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::DragFloat3("Position", &node.position.x, 0.01f)) changed = true;
			if (ImGui::DragFloat3("Rotation", &node.rotation.x, 0.5f))  changed = true;
			if (ImGui::DragFloat3("Scale",    &node.scale.x,    0.01f, 0.001f, 100.f)) changed = true;
		}

		if (m_readonly) ImGui::EndDisabled();

		ImGui::Separator();

		// Components — show only overrides, not merged parent data
		for (auto& [key, comp] : node.components) {
			// Resolve type name for header
			std::string effective_type = comp.type_name;
			if (effective_type.empty() && comp.parent_desc.is_valid() && comp.parent_desc.is_ready()) {
				effective_type = std::string(comp.parent_desc->get_type());
			}

			const std::string& header = effective_type.empty() ? key : effective_type;
			std::string header_id = header + "##" + key;
			if (ImGui::CollapsingHeader(header_id.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
				if (m_readonly) ImGui::BeginDisabled();

				// Show source tag if component references a parent desc
				if (comp.parent_desc.is_valid() && comp.parent_desc.is_ready()) {
					ImGui::TextDisabled("Source: %s", std::string(comp.parent_desc->get_tag().view()).c_str());
				}

				if (!comp.overrides.empty()) {
					// Has overrides — editable path
					bool handled = false;
					if (m_ui_registry && !effective_type.empty()) {
						auto result = m_ui_registry->invoke(effective_type, comp);
						if (result.has_value()) {
							if (*result && !m_readonly) {
								changed = true;
							}
							handled = true;
						}
					}
					if (!handled) {
						render_generic_json(comp.overrides);
					}
				} else if (auto it = m_parent_cache.find(key); it != m_parent_cache.end()) {
					// No overrides but has parent_desc — show cached parent data read-only
					render_generic_json_readonly(it->second);
				}

				if (m_readonly) ImGui::EndDisabled();
			}
		}

		ImGui::Separator();
		if (!m_readonly)
			draw_add_component_popup();

		if (changed && !m_readonly && m_on_node_changed)
			m_on_node_changed();
	}

	void inspector_panel::render_generic_json(boost::json::object& obj)
	{
		for (auto& [key, val] : obj) {
			std::string k(key);
			if (k.starts_with("__")) continue; // skip meta fields
			if (k == "children") continue; // shown in hierarchy panel

			render_generic_json_value(k, val);
		}
	}

	void inspector_panel::render_generic_json_value(const std::string& key, boost::json::value& val)
	{
		if (val.is_double()) {
			float f = static_cast<float>(val.as_double());
			if (ImGui::DragFloat(key.c_str(), &f, 0.01f))
				val = static_cast<double>(f);
		} else if (val.is_int64()) {
			int i = static_cast<int>(val.as_int64());
			if (ImGui::DragInt(key.c_str(), &i))
				val = static_cast<int64_t>(i);
		} else if (val.is_uint64()) {
			int i = static_cast<int>(val.as_uint64());
			if (ImGui::DragInt(key.c_str(), &i, 1.f, 0))
				val = static_cast<uint64_t>(std::max(0, i));
		} else if (val.is_string()) {
			ImGui::TextDisabled("%s: %s", key.c_str(), std::string(val.as_string()).c_str());
		} else if (val.is_bool()) {
			bool b = val.as_bool();
			if (ImGui::Checkbox(key.c_str(), &b))
				val = b;
		} else if (val.is_array()) {
			auto& arr = val.as_array();
			// vec3-like array of 3 numbers
			if (arr.size() == 3 && arr[0].is_number() && arr[1].is_number() && arr[2].is_number()) {
				glm::vec3 v{
					static_cast<float>(arr[0].is_double() ? arr[0].as_double() : static_cast<double>(arr[0].to_number<int64_t>())),
					static_cast<float>(arr[1].is_double() ? arr[1].as_double() : static_cast<double>(arr[1].to_number<int64_t>())),
					static_cast<float>(arr[2].is_double() ? arr[2].as_double() : static_cast<double>(arr[2].to_number<int64_t>()))
				};
				if (ImGui::DragFloat3(key.c_str(), &v.x, 0.01f)) {
					arr[0] = static_cast<double>(v.x);
					arr[1] = static_cast<double>(v.y);
					arr[2] = static_cast<double>(v.z);
				}
			} else {
				// Generic array — show as collapsible list with limit for large arrays
				constexpr std::size_t max_display = 50;
				std::string header = std::format("{} ({} items)", key, arr.size());
				if (ImGui::TreeNode(header.c_str())) {
					std::size_t count = std::min(arr.size(), max_display);
					for (std::size_t i = 0; i < count; ++i) {
						std::string item_key = std::format("[{}]", i);
						render_generic_json_value(item_key, arr[i]);
					}
					if (arr.size() > max_display) {
						ImGui::TextDisabled("... and %zu more items", arr.size() - max_display);
					}
					ImGui::TreePop();
				}
			}
		} else if (val.is_object()) {
			if (ImGui::TreeNode(key.c_str())) {
				auto& obj = val.as_object();
				for (auto& [k, v] : obj) {
					std::string sk(k);
					if (sk.starts_with("__")) continue;
					render_generic_json_value(sk, v);
				}
				ImGui::TreePop();
			}
		} else if (val.is_null()) {
			ImGui::TextDisabled("%s: null", key.c_str());
		}
	}

	void inspector_panel::render_generic_json_readonly(const boost::json::object& obj)
	{
		for (auto const& [key, val] : obj) {
			std::string k(key);
			if (k.starts_with("__")) continue;
			if (k == "children") continue;
			render_generic_json_value_readonly(k, val);
		}
	}

	void inspector_panel::render_generic_json_value_readonly(const std::string& key, const boost::json::value& val)
	{
		if (val.is_number()) {
			double d = val.is_double() ? val.as_double() : static_cast<double>(val.to_number<int64_t>());
			ImGui::TextDisabled("%s: %.4g", key.c_str(), d);
		} else if (val.is_string()) {
			ImGui::TextDisabled("%s: %s", key.c_str(), std::string(val.as_string()).c_str());
		} else if (val.is_bool()) {
			ImGui::TextDisabled("%s: %s", key.c_str(), val.as_bool() ? "true" : "false");
		} else if (val.is_array()) {
			auto const& arr = val.as_array();
			if (arr.size() == 3 && arr[0].is_number() && arr[1].is_number() && arr[2].is_number()) {
				float x = static_cast<float>(arr[0].is_double() ? arr[0].as_double() : static_cast<double>(arr[0].to_number<int64_t>()));
				float y = static_cast<float>(arr[1].is_double() ? arr[1].as_double() : static_cast<double>(arr[1].to_number<int64_t>()));
				float z = static_cast<float>(arr[2].is_double() ? arr[2].as_double() : static_cast<double>(arr[2].to_number<int64_t>()));
				ImGui::TextDisabled("%s: [%.3f, %.3f, %.3f]", key.c_str(), x, y, z);
			} else {
				constexpr std::size_t max_display = 50;
				std::string header = std::format("{} ({} items)", key, arr.size());
				if (ImGui::TreeNode(header.c_str())) {
					std::size_t count = std::min(arr.size(), max_display);
					for (std::size_t i = 0; i < count; ++i) {
						std::string item_key = std::format("[{}]", i);
						render_generic_json_value_readonly(item_key, arr[i]);
					}
					if (arr.size() > max_display) {
						ImGui::TextDisabled("... and %zu more items", arr.size() - max_display);
					}
					ImGui::TreePop();
				}
			}
		} else if (val.is_object()) {
			if (ImGui::TreeNode(key.c_str())) {
				auto const& obj = val.as_object();
				for (auto const& [k, v] : obj) {
					std::string sk(k);
					if (sk.starts_with("__")) continue;
					render_generic_json_value_readonly(sk, v);
				}
				ImGui::TreePop();
			}
		} else if (val.is_null()) {
			ImGui::TextDisabled("%s: null", key.c_str());
		}
	}

	void inspector_panel::draw_add_component_popup()
	{
		if (!m_selected_node) return;

		if (ImGui::Button("+ Add Component"))
			ImGui::OpenPopup("add_comp_desc");

		if (ImGui::BeginPopup("add_comp_desc")) {
			bool changed = false;
			if (m_desc_system) {
				for (const auto& type_name : m_desc_system->get_component_type_names()) {
					if (!m_selected_node->components.count(type_name)) {
						if (ImGui::MenuItem(type_name.c_str())) {
							scn::prefab_desc::prefab_comp_node comp;
							comp.type_name = type_name;
							m_selected_node->components[type_name] = std::move(comp);
							changed = true;
						}
					}
				}
			}
			ImGui::EndPopup();
			if (changed && m_on_node_changed)
				m_on_node_changed();
		}
	}
}
