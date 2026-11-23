#include "edt_document_tabs_bar.h"
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <string>

void edt::document_tabs_bar::draw(const std::vector<document_tab>& tabs, const gui::theme_semantic_colors& colors, const actions& on)
{
	if (tabs.empty()) return;
	document_id active_id = 0;
	for (const document_tab& tab : tabs) {
		if (tab.active) active_id = tab.id;
	}
	if (!ImGui::BeginTabBar("##document_tabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTabListScrollingButtons)) return;
	document_id selected_now = 0;
	document_id closed = 0;
	for (const document_tab& tab : tabs) {
		ImGuiTabItemFlags flags = ImGuiTabItemFlags_NoPushId;
		if (tab.dirty) flags |= ImGuiTabItemFlags_UnsavedDocument;
		if (tab.id == active_id && active_id != m_selected) flags |= ImGuiTabItemFlags_SetSelected;
		const std::string label = tab.title + "###doc" + std::to_string(tab.id);
		bool open = true;
		if (tab.dirty) ImGui::PushStyleColor(ImGuiCol_Text, colors.warning);
		const bool selected = ImGui::BeginTabItem(label.c_str(), tab.closable ? &open : nullptr, flags);
		if (tab.dirty) ImGui::PopStyleColor();
		if (selected) {
			selected_now = tab.id;
			ImGui::EndTabItem();
		}
		if (!open) closed = tab.id;
	}
	ImGui::EndTabBar();

	if (closed != 0 && on.close) on.close(closed);
	if (m_selected != 0 && selected_now != 0 && selected_now != active_id && selected_now != m_selected && on.activate) on.activate(selected_now);
	m_selected = selected_now != 0 ? selected_now : active_id;
}
