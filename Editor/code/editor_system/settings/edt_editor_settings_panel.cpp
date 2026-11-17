#include "edt_editor_settings_panel.h"
#include "edt_theme_settings.h"

#include <imgui.h>

#include <format>
#include <string>

namespace
{
	std::string label_of(const res::tag& preset)
	{
		const std::string name = edt::theme_name_of(preset);
		return edt::is_personal_theme(preset) ? name : name + " (built-in)";
	}

	std::string font_label_of(const res::tag& font)
	{
		return font.view().starts_with(edt::k_personal_fonts) ? edt::theme_name_of(font) + " (edt://fonts/)"
			: std::string{ "Noto Sans (UI font)" };
	}
}

edt::editor_settings_panel::editor_settings_panel(theme_settings_model& model)
	: panel_base(k_id, k_title)
	, m_model(model)
{
}

void edt::editor_settings_panel::on_before_begin()
{
	ImGui::SetNextWindowSize(ImVec2(520.f, 640.f), ImGuiCond_FirstUseEver);
	if (m_focus) {
		ImGui::SetNextWindowFocus();
		m_focus = false;
	}
}

void edt::editor_settings_panel::on_render()
{
	if (!m_model.available()) {
		ImGui::TextDisabled("There is no renderer (headless): the theme cannot be changed.");
		return;
	}
	ImGui::SeparatorText("Theme");
	draw_theme_list();
	ImGui::SeparatorText("Preset");
	draw_preset_editor();
	if (!m_model.last_message().empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", m_model.last_message().c_str());
	}
}

void edt::editor_settings_panel::draw_theme_list()
{
	const res::tag current = m_model.current();
	if (ImGui::BeginCombo("##theme", label_of(current).c_str())) {
		for (const res::tag& preset : m_model.presets()) {
			const bool selected = preset == current;
			if (ImGui::Selectable(label_of(preset).c_str(), selected) && !selected) {
				m_model.choose(preset);
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh")) {
		m_model.refresh();
	}
	ImGui::TextDisabled("Your presets: %s   Your fonts (.ttf, .otf): %s", k_personal_themes.data(),
		k_personal_fonts.data());
}

void edt::editor_settings_panel::draw_preset_editor()
{
	gui::theme_desc* preset = m_model.working();
	if (!preset) {
		ImGui::TextDisabled("Loading %s...", m_model.working_tag().view().data());
		return;
	}
	if (!m_model.editable()) {
		ImGui::TextWrapped("A built-in preset cannot be changed. Duplicate it to edit your own copy.");
		draw_duplicate();
		return;
	}

	bool changed = false;
	bool classic = preset->base == gui::theme_base::imgui_dark;
	if (ImGui::Checkbox("ImGui's own dark colors and sizes", &classic)) {
		preset->base = classic ? gui::theme_base::imgui_dark : gui::theme_base::palette;
		changed = true;
	}
	if (!classic) {
		changed |= ImGui::Checkbox("Light (dark text on light panels)", &preset->light);
		if (ImGui::TreeNodeEx("Colors", ImGuiTreeNodeFlags_DefaultOpen)) {
			const std::pair<const char*, glm::vec3*> colors[] = {
				{ "Window", &preset->window }, { "Panel", &preset->panel }, { "Frame", &preset->frame },
				{ "Frame hover", &preset->frame_hover }, { "Border", &preset->border }, { "Text", &preset->text },
				{ "Text dim", &preset->text_dim }, { "Accent", &preset->accent },
				{ "Accent hover", &preset->accent_hover }, { "Selection", &preset->selection },
			};
			for (const auto& [name, color] : colors) {
				changed |= ImGui::ColorEdit3(name, &color->x, ImGuiColorEditFlags_DisplayHex);
			}
			ImGui::TreePop();
		}
		if (ImGui::TreeNodeEx("Sizes", ImGuiTreeNodeFlags_DefaultOpen)) {
			changed |= ImGui::SliderFloat("Rounding", &preset->rounding, 0.f, 16.f, "%.0f");
			changed |= ImGui::SliderFloat("Scrollbar rounding", &preset->scrollbar_rounding, 0.f, 16.f, "%.0f");
			changed |= ImGui::SliderFloat2("Window padding", &preset->window_padding.x, 0.f, 32.f, "%.0f");
			changed |= ImGui::SliderFloat2("Frame padding", &preset->frame_padding.x, 0.f, 32.f, "%.0f");
			changed |= ImGui::SliderFloat2("Item spacing", &preset->item_spacing.x, 0.f, 32.f, "%.0f");
			changed |= ImGui::SliderFloat("Scrollbar size", &preset->scrollbar_size, 4.f, 32.f, "%.0f");
			changed |= ImGui::SliderFloat("Border size", &preset->border_size, 0.f, 4.f, "%.0f");
			changed |= ImGui::SliderFloat("Tab line", &preset->tab_overline, 0.f, 8.f, "%.0f");
			ImGui::TreePop();
		}
	}
	if (ImGui::TreeNodeEx("Font", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginCombo("Font", font_label_of(preset->font).c_str())) {
			for (const res::tag& font : m_model.fonts()) {
				if (ImGui::Selectable(font_label_of(font).c_str(), font == preset->font)) {
					preset->font = font;
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		// The atlas is rebuilt for a new size: the change is applied when the slider is let go.
		ImGui::SliderFloat("Size", &preset->font_size, 8.f, 48.f, "%.0f px");
		changed |= ImGui::IsItemDeactivatedAfterEdit();
		changed |= ImGui::Checkbox("Icons", &preset->icons);
		ImGui::TreePop();
	}
	if (changed) {
		m_model.edited();
	}

	ImGui::Spacing();
	ImGui::BeginDisabled(!m_model.dirty());
	if (ImGui::Button("Save")) {
		m_model.save();
	}
	ImGui::SameLine();
	if (ImGui::Button("Revert")) {
		m_model.revert();
	}
	ImGui::EndDisabled();
	draw_duplicate();
}

void edt::editor_settings_panel::draw_duplicate()
{
	ImGui::Spacing();
	ImGui::SetNextItemWidth(220.f);
	ImGui::InputTextWithHint("##new_name", "name of the copy", m_new_name.data(), m_new_name.size());
	ImGui::SameLine();
	if (ImGui::Button("Duplicate") && !m_model.duplicate(m_new_name.data())) {
		m_new_name.fill('\0');
	}
}
