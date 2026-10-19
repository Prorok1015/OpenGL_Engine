#include "edt_dockspace.h"
#include "edt_main_menu.h"
#include <imgui_internal.h>
#include <algorithm>

namespace edt
{
	void dockspace::render(panel_registry& panels, action_registry& actions)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);

		ImGuiWindowFlags host_flags =
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoNavFocus |
			ImGuiWindowFlags_MenuBar;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
		ImGui::Begin("##dockspace_host", nullptr, host_flags);
		ImGui::PopStyleVar(3);

		if (ImGui::BeginMenuBar())
		{
			draw_main_menu(actions, panels);
			ImGui::EndMenuBar();
		}

		ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
		ImGui::DockSpace(dockspace_id, ImVec2(0.f, 0.f), ImGuiDockNodeFlags_PassthruCentralNode);

		if (!m_layout_initialized)
		{
			setup_default_layout(dockspace_id, panels.panels());
			m_layout_initialized = true;
		}

		ImGui::End();

		panels.render_all();
	}

	void dockspace::setup_default_layout(ImGuiID dockspace_id, const std::vector<panel_info>& panels)
	{
		ImGui::DockBuilderRemoveNode(dockspace_id);
		ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

		ImGuiID dock_center = dockspace_id;
		ImGuiID dock_left   = ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Left,  0.20f, nullptr, &dock_center);
		ImGuiID dock_right  = ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Right, 0.25f, nullptr, &dock_center);
		ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Down,  0.28f, nullptr, &dock_center);

		ImGuiID dock_bottom_right;
		ImGuiID dock_bottom_left = ImGui::DockBuilderSplitNode(dock_bottom, ImGuiDir_Left, 0.50f, nullptr, &dock_bottom_right);

		// The first center panel owns the center. Another one that is open now sits side by side with it; a closed
		// one joins it as a tab when first shown (a split would leave an empty node while the panel is closed).
		std::vector<const panel_info*> beside;
		bool center_taken = false;
		for (const panel_info& panel : panels) {
			if (panel.slot != dock_slot::center) continue;
			if (center_taken && panel.open) {
				ImGuiID dock_side = ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Right, 0.50f, nullptr, &dock_center);
				ImGui::DockBuilderDockWindow(panel.title.c_str(), dock_side);
				beside.push_back(&panel);
			}
			center_taken = true;
		}

		for (const panel_info& panel : panels) {
			switch (panel.slot) {
			case dock_slot::left:         ImGui::DockBuilderDockWindow(panel.title.c_str(), dock_left); break;
			case dock_slot::right:        ImGui::DockBuilderDockWindow(panel.title.c_str(), dock_right); break;
			case dock_slot::bottom_left:  ImGui::DockBuilderDockWindow(panel.title.c_str(), dock_bottom_left); break;
			case dock_slot::bottom_right: ImGui::DockBuilderDockWindow(panel.title.c_str(), dock_bottom_right); break;
			case dock_slot::center:
				if (std::find(beside.begin(), beside.end(), &panel) == beside.end())
					ImGui::DockBuilderDockWindow(panel.title.c_str(), dock_center);
				break;
			case dock_slot::floating:     break;
			}
		}

		ImGui::DockBuilderFinish(dockspace_id);
	}
}
