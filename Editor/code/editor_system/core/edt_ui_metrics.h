#pragma once
#include <imgui.h>

namespace edt
{
	// The pixel sizes of the panels were tuned at 13 px, ImGui's default font (the classic theme): they scale with the
	// font, so a larger font size or DPI keeps the layout (#1136).
	constexpr float em_of(float px, float font_px)
	{
		return px * font_px / 13.f;
	}

	inline float em(float px)
	{
		return em_of(px, ImGui::GetFontSize());
	}

	inline ImVec2 em(ImVec2 px)
	{
		return ImVec2(em(px.x), em(px.y));
	}

	// A size that follows the DPI only, not the font size: the layout of the default view stays as it is.
	inline float dpi_px(float px)
	{
		return px * ImGui::GetMainViewport()->DpiScale;
	}
}
