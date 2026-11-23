#include "edt_shape_menu.h"
#include "edt_mesh_base_shapes.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <algorithm>
#include <vector>

namespace edt
{
	namespace
	{
		constexpr const char* dialog_title = "Add Parametric Shape";

		void draw_text_field(const char* label, std::string& text, bool focus)
		{
			std::vector<char> buffer(std::max<std::size_t>(text.size() + 64, 256), '\0');
			std::copy(text.begin(), text.end(), buffer.begin());
			if (focus) ImGui::SetKeyboardFocusHere();
			ImGui::InputText(label, buffer.data(), buffer.size());
			if (text != buffer.data()) text = buffer.data();
		}

		void draw_field(mesh::shape_field& field)
		{
			ImGui::PushID(field.key.c_str());
			for (std::size_t i = 0; i < field.values.size(); ++i) {
				ImGui::PushID(static_cast<int>(i));
				if (i > 0) ImGui::SameLine();
				ImGui::SetNextItemWidth(em(70.f));
				ImGui::InputDouble("##value", &field.values[i], 0.0, 0.0, field.integer ? "%.0f" : "%.3f");
				ImGui::PopID();
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(field.key.c_str());
			ImGui::PopID();
		}
	}

	void draw_shape_fields(mesh::parametric_shape_form& form)
	{
		for (mesh::shape_field& field : form.fields()) draw_field(field);
	}

	void shape_menu::draw_menu(const std::vector<std::uint64_t>& parent, const on_pick& pick)
	{
		if (!ImGui::BeginMenu("Add shape")) return;
		for (const mesh::base_shape& shape : mesh::base_shapes()) {
			if (ImGui::MenuItem(std::string(shape.name).c_str())) {
				pick({ std::string(shape.name), {}, parent });
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Parametric...")) {
			m_open_requested = true;
			m_parent = parent;
			m_problem.clear();
		}
		ImGui::EndMenu();
	}

	void shape_menu::draw_dialog(const gui::theme_semantic_colors& colors, const on_pick& pick)
	{
		if (m_open_requested) {
			ImGui::OpenPopup(dialog_title);
			m_open_requested = false;
		}
		if (!ImGui::BeginPopupModal(dialog_title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

		if (ImGui::BeginCombo("Kind", std::string(m_form.kind()).c_str())) {
			for (const std::string_view kind : mesh::shape_kinds) {
				if (ImGui::Selectable(std::string(kind).c_str(), kind == m_form.kind())) m_form.set_kind(kind);
			}
			ImGui::EndCombo();
		}
		draw_text_field("Name", m_form.name, ImGui::IsWindowAppearing());
		draw_text_field("Folder", m_form.folder, false);
		ImGui::TextDisabled("Files land in res://<folder>/<name>.*; the folder is shapes when empty");
		ImGui::Separator();
		draw_shape_fields(m_form);

		if (!m_problem.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, colors.error);
			ImGui::TextWrapped("%s", m_problem.c_str());
			ImGui::PopStyleColor();
		}
		if (ImGui::Button("Create") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
			m_problem = m_form.problem();
			if (m_problem.empty()) {
				ImGui::CloseCurrentPopup();
				pick({ {}, m_form.create_args(), m_parent });
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}
