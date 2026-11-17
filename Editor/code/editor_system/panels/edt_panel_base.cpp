#include <snk/editor/edt_panel_base.h>
#include <snk/gui/gui_theme.h>
#include <imgui.h>

namespace edt
{
	panel_base::panel_base(std::string_view id, std::string_view title)
		: m_id(id), m_title(title)
	{
	}

	void panel_base::set_colors(const gui::theme_semantic_colors* colors)
	{
		m_colors = colors;
	}

	const gui::theme_semantic_colors& panel_base::colors() const
	{
		static const gui::theme_semantic_colors defaults;
		return m_colors ? *m_colors : defaults;
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
