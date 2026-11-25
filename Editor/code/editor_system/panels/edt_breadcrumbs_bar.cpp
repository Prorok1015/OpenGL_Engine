#include "edt_breadcrumbs_bar.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <cstring>
#include <string>

namespace
{
	constexpr const char* k_payload_type = "snk.link";
	// The dock host window of edt_dockspace.cpp: a crumb let go over it (and over nothing else) is let go on empty space.
	constexpr const char* k_dockspace_host_name = "##dockspace_host";

	bool over_empty_space()
	{
		const ImGuiWindow* hovered = ImGui::GetCurrentContext()->HoveredWindow;
		return !hovered || std::strcmp(hovered->Name, k_dockspace_host_name) == 0;
	}
}

void edt::breadcrumbs_bar::draw(const std::vector<breadcrumb>& rows, host_id host, bool allow_move, const gui::theme_semantic_colors& colors, const actions& on)
{
	if (rows.empty()) return;
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float row_height = ImGui::GetFrameHeight();
	const float width = ImGui::GetContentRegionAvail().x;

	for (std::size_t i = 0; i < rows.size(); ++i) {
		const breadcrumb& crumb = rows[i];
		if (i > 0) {
			ImGui::SameLine(0.f, em(4.f));
			ImGui::TextColored(colors.text_secondary, ">");
			ImGui::SameLine(0.f, em(4.f));
		}
		const std::string text = crumb.dirty ? crumb.label + " *" : crumb.label;
		if (crumb.dirty) ImGui::PushStyleColor(ImGuiCol_Text, colors.warning);
		if (crumb.current || crumb.id == 0) {
			ImGui::TextUnformatted(text.c_str());
		} else if (ImGui::SmallButton((text + "###crumb" + std::to_string(crumb.id)).c_str()) && on.navigate) {
			on.navigate(crumb.id);
		}
		if (crumb.dirty) ImGui::PopStyleColor();

		if (allow_move && crumb.draggable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
			ImGui::SetDragDropPayload(k_payload_type, &crumb.id, sizeof(crumb.id));
			ImGui::TextUnformatted(crumb.label.c_str());
			m_dragged = crumb.id;
			ImGui::EndDragDropSource();
		}
	}

	const ImRect bar(origin, ImVec2(origin.x + width, origin.y + row_height));
	if (ImGui::BeginDragDropTargetCustom(bar, ImGui::GetID("##crumb_bar"))) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(k_payload_type)) {
			document_id link = 0;
			std::memcpy(&link, payload->Data, sizeof(link));
			m_accepted = true;
			if (on.dropped) on.dropped(link, { drop_kind::breadcrumb_bar, host });
		}
		ImGui::EndDragDropTarget();
	}

	if (m_dragged != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		if (!m_accepted && over_empty_space() && on.dropped) on.dropped(m_dragged, { drop_kind::empty_space, 0 });
		m_dragged = 0;
		m_accepted = false;
	}
}
