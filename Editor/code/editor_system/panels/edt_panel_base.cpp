#include "edt_panel_base.h"
#include <imgui.h>

namespace edt
{
	panel_base::panel_base(std::string_view id, std::string_view title)
		: m_id(id), m_title(title)
	{
	}

	void panel_base::render()
	{
		if (!m_open) {
			on_hidden();
			return;
		}
		on_before_begin();
		if (ImGui::Begin(m_title.c_str(), &m_open, m_flags))
			on_render();
		else
			on_hidden();
		ImGui::End();
	}
}
