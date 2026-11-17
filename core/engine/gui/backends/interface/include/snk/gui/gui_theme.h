#pragma once
#include <imgui.h>

namespace gui
{
	// What the editor's panels draw states with instead of literal colours (#1136). Taken whatever imgui_dark says, so
	// the classic look has them too; default constructed they are the colours the panels used before.
	struct theme_semantic_colors {
		ImVec4 error{ 1.f, 0.3f, 0.3f, 1.f };
		ImVec4 warning{ 1.f, 0.8f, 0.f, 1.f };
		ImVec4 success{ 0.45f, 0.8f, 0.45f, 1.f };
		ImVec4 info{ 0.45f, 0.75f, 1.f, 1.f };
		ImVec4 text_secondary{ 0.6f, 0.6f, 0.6f, 1.f };
		// What an agent did or asks about.
		ImVec4 agent{ 0.95f, 0.75f, 0.3f, 1.f };
		ImVec4 diff_added{ 0.45f, 0.8f, 0.45f, 1.f };
		ImVec4 diff_removed{ 0.9f, 0.45f, 0.45f, 1.f };
		ImVec4 diff_hunk{ 0.45f, 0.7f, 0.95f, 1.f };
		// Behind a full-window image (the viewport, the game view).
		ImVec4 backdrop{ 0.f, 0.f, 0.f, 1.f };
	};

	// The look a theme preset (gui::theme_desc, #1109) resolves to, without the font bytes. Default constructed it is
	// the classic look: ImGui's dark style with the UI font at 13 px, what the smoke references were taken with.
	struct theme_palette {
		ImVec4 window{ 0.118f, 0.122f, 0.133f, 1.f };
		ImVec4 panel{ 0.149f, 0.157f, 0.169f, 1.f };
		ImVec4 frame{ 0.204f, 0.212f, 0.231f, 1.f };
		ImVec4 frame_hover{ 0.247f, 0.259f, 0.282f, 1.f };
		ImVec4 border{ 0.224f, 0.231f, 0.251f, 1.f };
		ImVec4 text{ 0.875f, 0.882f, 0.898f, 1.f };
		ImVec4 text_dim{ 0.525f, 0.541f, 0.569f, 1.f };
		ImVec4 accent{ 0.239f, 0.482f, 0.878f, 1.f };
		ImVec4 accent_hover{ 0.333f, 0.565f, 0.933f, 1.f };
		ImVec4 selection{ 0.180f, 0.263f, 0.420f, 1.f };
		bool light = false;
		theme_semantic_colors semantic;
	};

	struct theme_metrics {
		float rounding = 4.f;
		float scrollbar_rounding = 6.f;
		ImVec2 window_padding{ 8.f, 8.f };
		ImVec2 frame_padding{ 8.f, 4.f };
		ImVec2 item_spacing{ 8.f, 6.f };
		float scrollbar_size = 12.f;
		float border_size = 1.f;
		float tab_overline = 2.f;
	};

	struct theme_style {
		// ImGui's own dark colors and sizes; palette and metrics are not used.
		bool imgui_dark = true;
		theme_palette palette;
		theme_metrics metrics;
		// Before the DPI scale.
		float font_px = 13.f;
	};

	namespace detail
	{
		inline ImVec4 with_alpha(ImVec4 c, float alpha)
		{
			c.w = alpha;
			return c;
		}

		inline void apply_palette(const theme_palette& p, ImGuiStyle& style)
		{
			ImVec4* c = style.Colors;
			const ImVec4 none{ 0.f, 0.f, 0.f, 0.f };
			const ImVec4 black{ 0.f, 0.f, 0.f, 1.f };
			const ImVec4 white{ 1.f, 1.f, 1.f, 1.f };
			c[ImGuiCol_Text] = p.text;
			c[ImGuiCol_TextDisabled] = p.text_dim;
			c[ImGuiCol_WindowBg] = p.panel;
			c[ImGuiCol_ChildBg] = none;
			c[ImGuiCol_PopupBg] = with_alpha(p.panel, 0.98f);
			c[ImGuiCol_Border] = p.border;
			c[ImGuiCol_BorderShadow] = none;
			c[ImGuiCol_FrameBg] = p.frame;
			c[ImGuiCol_FrameBgHovered] = p.frame_hover;
			c[ImGuiCol_FrameBgActive] = p.frame_hover;
			c[ImGuiCol_TitleBg] = p.window;
			c[ImGuiCol_TitleBgActive] = p.window;
			c[ImGuiCol_TitleBgCollapsed] = p.window;
			c[ImGuiCol_MenuBarBg] = p.window;
			c[ImGuiCol_ScrollbarBg] = none;
			c[ImGuiCol_ScrollbarGrab] = p.frame_hover;
			c[ImGuiCol_ScrollbarGrabHovered] = p.border;
			c[ImGuiCol_ScrollbarGrabActive] = p.accent;
			c[ImGuiCol_CheckMark] = p.accent;
			c[ImGuiCol_SliderGrab] = p.accent;
			c[ImGuiCol_SliderGrabActive] = p.accent_hover;
			c[ImGuiCol_Button] = p.frame;
			c[ImGuiCol_ButtonHovered] = p.frame_hover;
			c[ImGuiCol_ButtonActive] = p.selection;
			c[ImGuiCol_Header] = p.selection;
			c[ImGuiCol_HeaderHovered] = p.frame_hover;
			c[ImGuiCol_HeaderActive] = p.selection;
			c[ImGuiCol_Separator] = p.border;
			c[ImGuiCol_SeparatorHovered] = p.accent_hover;
			c[ImGuiCol_SeparatorActive] = p.accent;
			c[ImGuiCol_ResizeGrip] = none;
			c[ImGuiCol_ResizeGripHovered] = p.accent_hover;
			c[ImGuiCol_ResizeGripActive] = p.accent;
			c[ImGuiCol_Tab] = p.window;
			c[ImGuiCol_TabHovered] = p.frame_hover;
			c[ImGuiCol_TabSelected] = p.panel;
			c[ImGuiCol_TabSelectedOverline] = p.accent;
			c[ImGuiCol_TabDimmed] = p.window;
			c[ImGuiCol_TabDimmedSelected] = p.panel;
			c[ImGuiCol_TabDimmedSelectedOverline] = p.border;
			c[ImGuiCol_DockingPreview] = with_alpha(p.accent, 0.5f);
			c[ImGuiCol_DockingEmptyBg] = p.window;
			c[ImGuiCol_PlotLines] = p.text_dim;
			c[ImGuiCol_PlotLinesHovered] = p.accent_hover;
			c[ImGuiCol_PlotHistogram] = p.accent;
			c[ImGuiCol_PlotHistogramHovered] = p.accent_hover;
			c[ImGuiCol_TableHeaderBg] = p.window;
			c[ImGuiCol_TableBorderStrong] = p.border;
			c[ImGuiCol_TableBorderLight] = p.frame;
			c[ImGuiCol_TableRowBg] = none;
			c[ImGuiCol_TableRowBgAlt] = with_alpha(p.light ? black : white, 0.03f);
			c[ImGuiCol_TextLink] = p.accent_hover;
			c[ImGuiCol_TextSelectedBg] = with_alpha(p.accent, 0.35f);
			c[ImGuiCol_DragDropTarget] = p.accent_hover;
			c[ImGuiCol_NavCursor] = p.accent;
			c[ImGuiCol_NavWindowingHighlight] = p.accent;
			c[ImGuiCol_NavWindowingDimBg] = with_alpha(black, 0.4f);
			c[ImGuiCol_ModalWindowDimBg] = with_alpha(black, p.light ? 0.25f : 0.5f);
		}

		inline void apply_metrics(const theme_metrics& m, ImGuiStyle& style)
		{
			style.WindowPadding = m.window_padding;
			style.FramePadding = m.frame_padding;
			style.ItemSpacing = m.item_spacing;
			style.ItemInnerSpacing = ImVec2(m.item_spacing.x * 0.75f, m.item_spacing.y * 0.75f);
			style.ScrollbarSize = m.scrollbar_size;
			style.WindowBorderSize = m.border_size;
			style.ChildBorderSize = m.border_size;
			style.PopupBorderSize = m.border_size;
			style.FrameBorderSize = 0.f;
			style.TabBorderSize = 0.f;
			style.TabBarBorderSize = m.border_size;
			style.TabBarOverlineSize = m.tab_overline;
			style.WindowRounding = m.rounding;
			style.ChildRounding = m.rounding;
			style.FrameRounding = m.rounding;
			style.PopupRounding = m.rounding;
			style.GrabRounding = m.rounding;
			style.TabRounding = m.rounding;
			style.ScrollbarRounding = m.scrollbar_rounding;
			style.WindowMenuButtonPosition = ImGuiDir_None;
		}
	}

	// Unscaled: the backend applies the DPI scale (ScaleAllSizes) after it.
	inline void apply_theme(const theme_style& theme, ImGuiStyle& style)
	{
		style = ImGuiStyle();
		ImGui::StyleColorsDark(&style);
		if (theme.imgui_dark) {
			return;
		}
		detail::apply_metrics(theme.metrics, style);
		detail::apply_palette(theme.palette, style);
	}
}
