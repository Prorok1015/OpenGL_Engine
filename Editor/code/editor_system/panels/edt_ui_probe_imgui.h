#pragma once
#include "edt_ui_probe.h"
#include <imgui.h>
#include <algorithm>
#include <string_view>

namespace edt
{
	// The ImGui side of ui_probe: what the last item drew, clipped to the window's clip rect (a row a list cuts off is
	// "visible" to ImGui; only its part inside counts, so a driver aiming at the centre does not land outside the panel).
	// A null probe does nothing.
	inline void probe_rect(ui_probe* probe, std::string_view area, std::string_view name, ImVec2 min, ImVec2 max, bool visible)
	{
		if (!probe) return;
		const ImDrawList* list = ImGui::GetWindowDrawList();
		min = ImVec2(std::max(min.x, list->GetClipRectMin().x), std::max(min.y, list->GetClipRectMin().y));
		max = ImVec2(std::min(max.x, list->GetClipRectMax().x), std::min(max.y, list->GetClipRectMax().y));
		if (visible && max.x - min.x >= 2.f && max.y - min.y >= 2.f) {
			probe->report(area, name, min.x, min.y, max.x, max.y);
		} else {
			probe->report_hidden(area, name);
		}
	}

	inline void probe_last_item(ui_probe* probe, std::string_view area, std::string_view name)
	{
		if (!probe) return;
		probe_rect(probe, area, name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::IsItemVisible());
	}

	// The expand arrow of the tree node just drawn: its left end, GetTreeNodeToLabelSpacing wide, the row's height.
	inline void probe_tree_arrow(ui_probe* probe, std::string_view area, std::string_view name)
	{
		if (!probe) return;
		const ImVec2 min = ImGui::GetItemRectMin();
		probe_rect(probe, area, name, min, ImVec2(min.x + ImGui::GetTreeNodeToLabelSpacing(), ImGui::GetItemRectMax().y),
			ImGui::IsItemVisible());
	}
}
