#pragma once
#include "edt_ui_probe.h"
#include <imgui.h>
#include <algorithm>
#include <string_view>

namespace edt
{
	// The ImGui side of ui_probe: what the last item drew, clipped to the window's clip rect (a row a list cuts off
	// is "visible" to ImGui; only its part inside counts, so a driver aiming at the centre stays inside the panel).
	// A null probe does nothing.
	inline void probe_rect_in(ui_probe* probe, std::string_view area, std::string_view name, ImVec2 min, ImVec2 max,
		bool visible, ImVec2 clip_min, ImVec2 clip_max)
	{
		if (!probe) return;
		min = ImVec2(std::max(min.x, clip_min.x), std::max(min.y, clip_min.y));
		max = ImVec2(std::min(max.x, clip_max.x), std::min(max.y, clip_max.y));
		if (visible && max.x - min.x >= 2.f && max.y - min.y >= 2.f) {
			probe->report(area, name, min.x, min.y, max.x, max.y);
		} else {
			probe->report_hidden(area, name);
		}
	}

	inline void probe_rect(
		ui_probe* probe, std::string_view area, std::string_view name, ImVec2 min, ImVec2 max, bool visible)
	{
		if (!probe) return;
		const ImDrawList* list = ImGui::GetWindowDrawList();
		probe_rect_in(probe, area, name, min, max, visible, list->GetClipRectMin(), list->GetClipRectMax());
	}

	inline void probe_last_item(ui_probe* probe, std::string_view area, std::string_view name)
	{
		if (!probe) return;
		probe_rect(probe, area, name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::IsItemVisible());
	}

	// The expand arrow of the tree node just drawn, from arrow_x (the cursor x before the node: a SpanFullWidth row's
	// rectangle starts at the panel's edge, not at its indent), GetTreeNodeToLabelSpacing wide, the row's height.
	inline void probe_tree_arrow(ui_probe* probe, std::string_view area, std::string_view name, float arrow_x)
	{
		if (!probe) return;
		const ImVec2 min(arrow_x, ImGui::GetItemRectMin().y);
		const ImVec2 max(arrow_x + ImGui::GetTreeNodeToLabelSpacing(), ImGui::GetItemRectMax().y);
		probe_rect(probe, area, name, min, max, ImGui::IsItemVisible());
	}
}
