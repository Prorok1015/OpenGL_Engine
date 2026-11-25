#include "edt_material_window_panel.h"
#include "edt_input_binding.h"
#include "edt_input_manager.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_system.h>
#include <snk/gui/gui_theme.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/scene/scn_camera_component.hpp>
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
	material_preview_view view)
	: data_document_panel(document, services, id, quiet)
	, m_model(document, services)
	, m_preview(document, services)
	, m_view(view)
{
	if (rnd::render_system* render = m_view.render) {
		m_preview.set_targets_release([render](const std::vector<res::tag>& tags) {
			auto& textures = render->get_texture_manager();
			for (const res::tag& tag : tags) textures.remove(tag);
		});
	}
}

void edt::material_window_panel::on_hidden()
{
	m_preview.park();
	data_document_panel::on_hidden();
}

void edt::material_window_panel::draw_before_fields()
{
	m_preview.update(m_model.shader());
	if (m_preview.sky_material()) {
		ImGui::TextDisabled("Sky material: no preview.");
		ImGui::Separator();
		return;
	}
	ImGui::SetNextItemWidth(em(110.f));
	if (ImGui::BeginCombo("Shape", std::string{ preview_shape_label(m_preview.shape()) }.c_str())) {
		for (const preview_shape shape : k_shapes) {
			const bool selected = shape == m_preview.shape();
			if (ImGui::Selectable(std::string{ preview_shape_label(shape) }.c_str(), selected)) {
				m_preview.set_shape(shape);
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	bool sky = m_preview.sky();
	if (ImGui::Checkbox("Sky", &sky)) {
		m_preview.set_sky(sky);
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The sky behind the shape, or the camera's plain clear.");
	}
	// Resizable from its bottom edge; the fields scroll under it.
	if (ImGui::BeginChild("##preview", ImVec2(0.f, em(220.f)), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		draw_preview_image();
	} else {
		m_preview.park();
	}
	ImGui::EndChild();
}

void edt::material_window_panel::draw_preview_image()
{
	const ImVec2 size = ImGui::GetContentRegionAvail();
	if (size.x <= 0 || size.y <= 0) {
		m_preview.park();
		return;
	}
	scn::camera_component* camera = m_preview.camera();
	if (!camera) {
		ImGui::TextDisabled("(no preview)");
		return;
	}
	if (!m_view.render || !m_view.gui) {
		m_preview.park();
		return;
	}
	const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
	camera->m_viewport.size = edt::to_framebuffer_size({ size.x, size.y }, { fb_scale.x, fb_scale.y });

	auto& textures = m_view.render->get_texture_manager();
	const res::tag target{ m_preview.render_target() };
	if (const auto texture = textures.binding(textures.id_of(target)).handle) {
		ImTextureID tex_id = m_view.gui->get_backend_interface()->to_imgui(texture);
		const ImVec2 image_pos = ImGui::GetCursorScreenPos();
		ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));
		ImGui::SetCursorScreenPos(image_pos);
		ImGui::InvisibleButton("##material_preview_input", size);
		bind_item_input(m_view.input, inp::input_target{ .set = m_preview.set_name(), .context = std::string{ material_preview::context_name() } });
	} else {
		ImGui::TextDisabled("(waiting for render target...)");
	}
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
