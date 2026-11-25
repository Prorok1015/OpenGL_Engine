#pragma once
#include <snk/gui/gui_theme.h>
#include <TextEditor.h>

namespace edt
{
	// What the code colours are taken from: the theme's semantic colours and the three ImGui style colours the text sits on. Nothing
	// here is a literal colour, so a theme change reaches the editor (#1136).
	struct shader_palette_source
	{
		gui::theme_semantic_colors semantic;
		ImVec4 text;
		ImVec4 background;
		ImVec4 selection;
	};

	// Reads the style's colours of the current ImGui context (`ImGuiCol_Text`, `ImGuiCol_WindowBg`, `ImGuiCol_TextSelectedBg`).
	shader_palette_source shader_palette_source_of_style(const gui::theme_semantic_colors& semantic);

	// The palette of the text widget: keywords, types, numbers, strings, comments and the preprocessor in the semantic colours, the rest in
	// the style's.
	TextEditor::Palette make_shader_palette(const shader_palette_source& source);

	// `a` moved towards `b` by `amount` (0..1), alpha 1.
	ImVec4 mix_colors(const ImVec4& a, const ImVec4& b, float amount);
}
