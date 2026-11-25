#include "edt_material_window_panel.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace
{
	constexpr std::array<edt::preview_shape, 3> k_shapes{ edt::preview_shape::sphere, edt::preview_shape::cube, edt::preview_shape::plane };
}

edt::material_window_panel::material_window_panel(data_document& document, editor_services& services, document_id id, bool quiet,
	preview_view view)
	: data_document_panel(document, services, id, quiet)
	, m_model(document, services)
	, m_content(document, m_model, services)
	, m_stage(services, m_content, view)
{
}

void edt::material_window_panel::on_hidden()
{
	m_stage.park();
	data_document_panel::on_hidden();
}

void edt::material_window_panel::draw_before_fields()
{
	m_stage.update();
	if (m_content.sky_material()) {
		ImGui::TextDisabled("Sky material: no preview.");
		ImGui::Separator();
		return;
	}
	ImGui::SetNextItemWidth(em(110.f));
	if (ImGui::BeginCombo("Shape", std::string{ preview_shape_label(m_content.shape()) }.c_str())) {
		for (const preview_shape shape : k_shapes) {
			const bool selected = shape == m_content.shape();
			if (ImGui::Selectable(std::string{ preview_shape_label(shape) }.c_str(), selected) && !selected) {
				m_content.set_shape(shape);
				m_stage.sync();
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	bool sky = m_stage.sky();
	if (ImGui::Checkbox("Sky", &sky)) {
		m_stage.set_sky(sky);
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The sky behind the shape, or the camera's plain clear.");
	}
	// Resizable from its bottom edge; the fields scroll under it.
	if (ImGui::BeginChild("##preview", ImVec2(0.f, em(220.f)), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		m_stage.draw_image();
	} else {
		m_stage.park();
	}
	ImGui::EndChild();
}

void edt::material_window_panel::draw_after_fields()
{
	ImGui::Spacing();
	ImGui::SeparatorText("Defines");
	ImGui::BeginDisabled(!m_model.can_reset_to_inherited());
	if (ImGui::SmallButton("Reset to inherited")) {
		m_model.reset_to_inherited();
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Drop this material's own defines: the parent's list applies again.");
	}
	const material_shader& shader = m_model.shader();
	if (shader.shader.is_valid()) {
		ImGui::TextDisabled("shader: %s", std::string{ shader.shader.view() }.c_str());
	} else {
		ImGui::TextDisabled("The material has no shader: only its own defines are listed.");
	}
	if (!shader.error.empty()) {
		ImGui::TextColored(colors().warning, "%s", shader.error.c_str());
	}

	const std::vector<define_row> rows = m_model.rows();
	std::optional<std::string> toggled;
	if (rows.empty()) {
		ImGui::TextDisabled("No defines: the shader tests none and the material sets none.");
	} else if (ImGui::BeginTable("##defines", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Define", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, em(110.f));
		ImGui::TableHeadersRow();
		for (const define_row& row : rows) {
			ImGui::TableNextRow();
			ImGui::PushID(row.name.c_str());
			ImGui::TableSetColumnIndex(0);
			bool on = row.state != define_state::off;
			if (ImGui::Checkbox(row.name.c_str(), &on)) {
				toggled = row.name;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", row.state == define_state::own_on ? "Set in this material's own defines."
					: row.state == define_state::inherited_on ? "Set by a parent material; turning it off writes this material's own list."
					: "Off.");
			}
			if (row.state == define_state::inherited_on) {
				ImGui::SameLine();
				ImGui::TextDisabled("(inherited)");
			} else if (row.inherited && row.state == define_state::off) {
				ImGui::SameLine();
				ImGui::TextDisabled("(inherited, off here)");
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("%s", row.from_shader ? "shader" : "material");
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	// After the table: the toggle changes the rows the loop drew.
	if (toggled) {
		m_model.toggle(*toggled);
	}
	draw_add_define();
}

void edt::material_window_panel::draw_add_define()
{
	ImGui::SetNextItemWidth(em(180.f));
	const bool entered = ImGui::InputTextWithHint("##new_define", "NEW_DEFINE", m_new_define, sizeof(m_new_define),
		ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	if (ImGui::Button("Add") || entered) {
		m_add_error = m_model.add(m_new_define);
		if (m_add_error.empty()) {
			m_new_define[0] = '\0';
		}
	}
	if (!m_add_error.empty()) {
		ImGui::TextColored(colors().error, "%s", m_add_error.c_str());
	}
}
