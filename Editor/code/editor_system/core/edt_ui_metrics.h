#pragma once
#include <imgui.h>

namespace edt
{
	// The pixel sizes of the panels were tuned at 13 px, ImGui's default font (the classic theme): they scale with the
	// font, so a larger font size or DPI keeps the layout (#1136).
	// One rule: dpi_px only for a size drawn in the default layout (the views the CI reference images show: hierarchy,
	// inspector, viewport, asset browser, problems, console, main menu), em for every other size.
	constexpr float em_of(float px, float font_px)
	{
		return px * font_px / 13.f;
	}

	// The default font's size, not the current window's: em() is also called before Begin (SetNextWindowSize).
	inline float ui_font_px()
	{
		const ImGuiIO& io = ImGui::GetIO();
		const ImFont* font = io.FontDefault ? io.FontDefault : (io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0]);
		return font ? font->FontSize * io.FontGlobalScale : 13.f;
	}

	inline float em(float px)
	{
		return em_of(px, ui_font_px());
	}

	inline ImVec2 em(ImVec2 px)
	{
		return ImVec2(em(px.x), em(px.y));
	}

	// A size that follows the DPI only, not the font size: the layout of the default view stays as it is.
	inline float dpi_px(float px)
	{
		const float scale = ImGui::GetMainViewport()->DpiScale;
		// Not set (tests, headless, a null backend): 1.
		return px * (scale > 0.f ? scale : 1.f);
	}
}
