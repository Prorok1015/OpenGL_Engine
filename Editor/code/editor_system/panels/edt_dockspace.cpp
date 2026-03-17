#include "edt_dockspace.h"
#include <imgui_internal.h>

namespace edt
{
	void dockspace::render(panel_manager& pm)
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

		render_menu_bar(pm);

		ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
		ImGui::DockSpace(dockspace_id, ImVec2(0.f, 0.f), ImGuiDockNodeFlags_PassthruCentralNode);

		if (!m_layout_initialized)
		{
			setup_default_layout(dockspace_id);
			m_layout_initialized = true;
		}

		ImGui::End();

		pm.render_all();
	}

	void dockspace::render_menu_bar(panel_manager& pm)
	{
		if (!ImGui::BeginMenuBar())
			return;

		// Ctrl+S shortcut (works regardless of menu focus)
		if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
			if (m_on_quick_save)
				m_on_quick_save();
			else if (m_on_save_level)
				m_on_save_level();
		}

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Level")     && m_on_new_level)   m_on_new_level();
			if (ImGui::MenuItem("Open Level...")  && m_on_open_level)  m_on_open_level();
			if (ImGui::MenuItem("Save Level", "Ctrl+S") && m_on_save_level)  m_on_save_level();
			ImGui::Separator();
			if (ImGui::MenuItem("Import Model...") && m_on_import_model) m_on_import_model();
			ImGui::Separator();
			if (ImGui::MenuItem("Exit")          && m_on_exit)        m_on_exit();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			pm.render_view_menu();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help"))
		{
			ImGui::MenuItem("About");
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}

	void dockspace::setup_default_layout(ImGuiID dockspace_id)
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

		ImGui::DockBuilderDockWindow("Scene Hierarchy", dock_left);
		ImGui::DockBuilderDockWindow("Inspector",       dock_right);
		ImGui::DockBuilderDockWindow("Viewport",        dock_center);
		ImGui::DockBuilderDockWindow("Asset Browser",   dock_bottom_left);
		ImGui::DockBuilderDockWindow("Console",         dock_bottom_right);

		ImGui::DockBuilderFinish(dockspace_id);
	}
}
