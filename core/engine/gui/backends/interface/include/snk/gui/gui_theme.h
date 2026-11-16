#pragma once
#include <imgui.h>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gui
{
	// classic is ImGui's own dark style with Noto 13 px, the look the smoke references were taken with (#1109):
	// it stays the default until the owner picks one of the others from the snapshots.
	enum class theme { classic, dark_neutral, dark_contrast, light };

	inline std::optional<theme> theme_from_name(std::string_view name)
	{
		if (name == "classic") return theme::classic;
		if (name == "dark_neutral") return theme::dark_neutral;
		if (name == "dark_contrast") return theme::dark_contrast;
		if (name == "light") return theme::light;
		return std::nullopt;
	}

	// UI font size before the DPI scale.
	inline float theme_font_px(theme t)
	{
		return t == theme::classic ? 13.f : 14.f;
	}

	namespace detail
	{
		struct theme_palette {
			uint32_t window;
			uint32_t panel;
			uint32_t frame;
			uint32_t frame_hover;
			uint32_t border;
			uint32_t text;
			uint32_t text_dim;
			uint32_t accent;
			uint32_t accent_hover;
			uint32_t selection;
			bool light;
		};

		inline ImVec4 rgb(uint32_t hex, float alpha = 1.f)
		{
			return ImVec4(((hex >> 16) & 0xff) / 255.f, ((hex >> 8) & 0xff) / 255.f, (hex & 0xff) / 255.f, alpha);
		}

		inline theme_palette palette_of(theme t)
		{
			switch (t) {
			case theme::dark_contrast:
				return {0x101113, 0x17181b, 0x24262b, 0x30333a, 0x3c4048, 0xf4f5f7, 0x8b9099,
					0xf0a43a, 0xffb85c, 0x5a4220, false};
			case theme::light:
				return {0xf3f4f6, 0xffffff, 0xe6e8ec, 0xd9dce2, 0xc9cdd4, 0x1f2328, 0x7a808a,
					0x2f6fde, 0x4a86ee, 0xcfe0fb, true};
			default:
				return {0x1e1f22, 0x26282b, 0x34363b, 0x3f4248, 0x393b40, 0xdfe1e5, 0x868a91,
					0x3d7be0, 0x5590ee, 0x2e436b, false};
			}
		}

		inline void apply_palette(const theme_palette& p, ImGuiStyle& style)
		{
			ImVec4* c = style.Colors;
			const ImVec4 accent = rgb(p.accent);
			const ImVec4 accent_hover = rgb(p.accent_hover);
			const ImVec4 selection = rgb(p.selection);
			c[ImGuiCol_Text] = rgb(p.text);
			c[ImGuiCol_TextDisabled] = rgb(p.text_dim);
			c[ImGuiCol_WindowBg] = rgb(p.panel);
			c[ImGuiCol_ChildBg] = rgb(p.panel, 0.f);
			c[ImGuiCol_PopupBg] = rgb(p.panel, 0.98f);
			c[ImGuiCol_Border] = rgb(p.border);
			c[ImGuiCol_BorderShadow] = rgb(0, 0.f);
			c[ImGuiCol_FrameBg] = rgb(p.frame);
			c[ImGuiCol_FrameBgHovered] = rgb(p.frame_hover);
			c[ImGuiCol_FrameBgActive] = rgb(p.frame_hover);
			c[ImGuiCol_TitleBg] = rgb(p.window);
			c[ImGuiCol_TitleBgActive] = rgb(p.window);
			c[ImGuiCol_TitleBgCollapsed] = rgb(p.window);
			c[ImGuiCol_MenuBarBg] = rgb(p.window);
			c[ImGuiCol_ScrollbarBg] = rgb(p.panel, 0.f);
			c[ImGuiCol_ScrollbarGrab] = rgb(p.frame_hover);
			c[ImGuiCol_ScrollbarGrabHovered] = rgb(p.border);
			c[ImGuiCol_ScrollbarGrabActive] = accent;
			c[ImGuiCol_CheckMark] = accent;
			c[ImGuiCol_SliderGrab] = accent;
			c[ImGuiCol_SliderGrabActive] = accent_hover;
			c[ImGuiCol_Button] = rgb(p.frame);
			c[ImGuiCol_ButtonHovered] = rgb(p.frame_hover);
			c[ImGuiCol_ButtonActive] = selection;
			c[ImGuiCol_Header] = selection;
			c[ImGuiCol_HeaderHovered] = rgb(p.frame_hover);
			c[ImGuiCol_HeaderActive] = selection;
			c[ImGuiCol_Separator] = rgb(p.border);
			c[ImGuiCol_SeparatorHovered] = accent_hover;
			c[ImGuiCol_SeparatorActive] = accent;
			c[ImGuiCol_ResizeGrip] = rgb(p.border, 0.f);
			c[ImGuiCol_ResizeGripHovered] = accent_hover;
			c[ImGuiCol_ResizeGripActive] = accent;
			c[ImGuiCol_Tab] = rgb(p.window);
			c[ImGuiCol_TabHovered] = rgb(p.frame_hover);
			c[ImGuiCol_TabSelected] = rgb(p.panel);
			c[ImGuiCol_TabSelectedOverline] = accent;
			c[ImGuiCol_TabDimmed] = rgb(p.window);
			c[ImGuiCol_TabDimmedSelected] = rgb(p.panel);
			c[ImGuiCol_TabDimmedSelectedOverline] = rgb(p.border);
			c[ImGuiCol_DockingPreview] = rgb(p.accent, 0.5f);
			c[ImGuiCol_DockingEmptyBg] = rgb(p.window);
			c[ImGuiCol_PlotLines] = rgb(p.text_dim);
			c[ImGuiCol_PlotLinesHovered] = accent_hover;
			c[ImGuiCol_PlotHistogram] = accent;
			c[ImGuiCol_PlotHistogramHovered] = accent_hover;
			c[ImGuiCol_TableHeaderBg] = rgb(p.window);
			c[ImGuiCol_TableBorderStrong] = rgb(p.border);
			c[ImGuiCol_TableBorderLight] = rgb(p.frame);
			c[ImGuiCol_TableRowBg] = rgb(0, 0.f);
			c[ImGuiCol_TableRowBgAlt] = p.light ? rgb(0x000000, 0.03f) : rgb(0xffffff, 0.03f);
			c[ImGuiCol_TextLink] = accent_hover;
			c[ImGuiCol_TextSelectedBg] = rgb(p.accent, 0.35f);
			c[ImGuiCol_DragDropTarget] = accent_hover;
			c[ImGuiCol_NavCursor] = accent;
			c[ImGuiCol_NavWindowingHighlight] = accent;
			c[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, 0.4f);
			c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, p.light ? 0.25f : 0.5f);
		}

		inline void apply_metrics(ImGuiStyle& style)
		{
			style.WindowPadding = ImVec2(8.f, 8.f);
			style.FramePadding = ImVec2(8.f, 4.f);
			style.ItemSpacing = ImVec2(8.f, 6.f);
			style.ItemInnerSpacing = ImVec2(6.f, 4.f);
			style.IndentSpacing = 16.f;
			style.ScrollbarSize = 12.f;
			style.GrabMinSize = 10.f;
			style.WindowBorderSize = 1.f;
			style.ChildBorderSize = 1.f;
			style.PopupBorderSize = 1.f;
			style.FrameBorderSize = 0.f;
			style.TabBorderSize = 0.f;
			style.TabBarBorderSize = 1.f;
			style.TabBarOverlineSize = 2.f;
			style.WindowRounding = 4.f;
			style.ChildRounding = 4.f;
			style.FrameRounding = 4.f;
			style.PopupRounding = 4.f;
			style.ScrollbarRounding = 6.f;
			style.GrabRounding = 3.f;
			style.TabRounding = 4.f;
			style.DockingSeparatorSize = 2.f;
			style.WindowMenuButtonPosition = ImGuiDir_None;
		}
	}

	// Unscaled: the backend applies the DPI scale (ScaleAllSizes) after it.
	inline void apply_theme(theme t, ImGuiStyle& style)
	{
		style = ImGuiStyle();
		if (t == theme::classic) {
			ImGui::StyleColorsDark(&style);
			return;
		}
		detail::apply_metrics(style);
		detail::apply_palette(detail::palette_of(t), style);
	}
}
