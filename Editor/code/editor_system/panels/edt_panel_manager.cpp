#include "edt_panel_manager.h"
#include <algorithm>
#include <imgui.h>

namespace edt
{
	void panel_manager::add_panel(std::shared_ptr<panel_base> panel)
	{
		m_panels.push_back(std::move(panel));
	}

	void panel_manager::remove_panel(std::string_view id)
	{
		auto it = std::find_if(m_panels.begin(), m_panels.end(),
			[id](const auto& p) { return p->get_id() == id; });
		if (it != m_panels.end())
			m_panels.erase(it);
	}

	panel_base* panel_manager::find_panel(std::string_view id) const
	{
		auto it = std::find_if(m_panels.begin(), m_panels.end(),
			[id](const auto& p) { return p->get_id() == id; });
		return it != m_panels.end() ? it->get() : nullptr;
	}

	void panel_manager::render_all()
	{
		for (auto& panel : m_panels)
			panel->render();
	}

	void panel_manager::render_view_menu()
	{
		for (auto& panel : m_panels)
		{
			bool open = panel->is_open();
			if (ImGui::MenuItem(panel->get_title().c_str(), nullptr, &open))
				panel->set_open(open);
		}
	}
}
