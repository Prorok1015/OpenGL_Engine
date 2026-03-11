#include "edt_asset_browser_panel.h"
#include <imgui.h>

namespace edt
{
	asset_browser_panel::asset_browser_panel()
		: panel_base("asset_browser", "Asset Browser")
	{
	}

	void asset_browser_panel::on_render()
	{
		ImGui::InputText("Search", m_search, sizeof(m_search));
		ImGui::Separator();

		ImGui::Columns(2, "asset_columns");

		// Left: directory list
		for (int i = 0; i < static_cast<int>(m_root_dirs.size()); ++i) {
			if (ImGui::Selectable(m_root_dirs[i].c_str(), m_selected_dir == i)) {
				m_selected_dir = i;
			}
		}

		ImGui::NextColumn();

		// Right: file list placeholder
		if (m_selected_dir >= 0 && m_selected_dir < static_cast<int>(m_root_dirs.size())) {
			ImGui::Text("Files in: %s", m_root_dirs[m_selected_dir].c_str());
			ImGui::TextDisabled("(not yet implemented)");
		}

		ImGui::Columns(1);
	}
}
