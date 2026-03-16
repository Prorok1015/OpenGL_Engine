#include "edt_panel_base.h"

namespace edt
{
	panel_base::panel_base(std::string_view id, std::string_view title)
		: m_id(id), m_title(title)
	{
	}

	void panel_base::render()
	{
		if (!m_open)
			return;
		if (ImGui::Begin(m_title.c_str(), &m_open, m_flags))
			on_render();
		ImGui::End();
	}
}
