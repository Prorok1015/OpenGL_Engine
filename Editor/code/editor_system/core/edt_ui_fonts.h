#pragma once
#include <snk/gui/gui_ui_font.h>
#include <imgui.h>

namespace edt
{
	// The faces of the UI font (#1136): headings in bold, logs, diffs and code in mono.
	enum class ui_font { text, bold, mono };
	static_assert(int(ui_font::text) == int(gui::ui_face::text) && int(ui_font::bold) == int(gui::ui_face::bold)
		&& int(ui_font::mono) == int(gui::ui_face::mono));

	// Draws with a face while it lives. The backend puts the faces at fixed indices of the atlas; an atlas without them
	// (the built-in font, a test's bare context) keeps the current font.
	class font_scope
	{
	public:
		explicit font_scope(ui_font face)
		{
			const ImFontAtlas* atlas = ImGui::GetIO().Fonts;
			const int index = static_cast<int>(face);
			if (atlas && index < atlas->Fonts.Size) {
				ImGui::PushFont(atlas->Fonts[index]);
				m_pushed = true;
			}
		}
		~font_scope()
		{
			if (m_pushed) {
				ImGui::PopFont();
			}
		}
		font_scope(const font_scope&) = delete;
		font_scope& operator=(const font_scope&) = delete;

	private:
		bool m_pushed = false;
	};

	// Section headings in the bold face.
	inline bool bold_header(const char* label, ImGuiTreeNodeFlags flags = 0)
	{
		const font_scope bold(ui_font::bold);
		return ImGui::CollapsingHeader(label, flags);
	}

	inline void bold_separator_text(const char* label)
	{
		const font_scope bold(ui_font::bold);
		ImGui::SeparatorText(label);
	}
}
