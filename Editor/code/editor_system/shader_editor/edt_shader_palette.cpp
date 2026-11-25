#include "edt_shader_palette.h"
#include <imgui.h>

namespace edt
{
	ImVec4 mix_colors(const ImVec4& a, const ImVec4& b, float amount)
	{
		return ImVec4(a.x + (b.x - a.x) * amount, a.y + (b.y - a.y) * amount, a.z + (b.z - a.z) * amount, 1.f);
	}

	shader_palette_source shader_palette_source_of_style(const gui::theme_semantic_colors& semantic)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		return shader_palette_source{ semantic, style.Colors[ImGuiCol_Text], style.Colors[ImGuiCol_WindowBg],
			style.Colors[ImGuiCol_TextSelectedBg] };
	}

	TextEditor::Palette make_shader_palette(const shader_palette_source& source)
	{
		using color = TextEditor::Color;
		const gui::theme_semantic_colors& s = source.semantic;
		const auto packed = [](const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); };
		const ImVec4 dim = mix_colors(source.text, source.background, 0.55f);
		const ImVec4 faint = mix_colors(source.text, source.background, 0.8f);

		TextEditor::Palette palette;
		palette[static_cast<size_t>(color::text)] = packed(source.text);
		palette[static_cast<size_t>(color::keyword)] = packed(s.info);
		palette[static_cast<size_t>(color::declaration)] = packed(s.diff_hunk);
		palette[static_cast<size_t>(color::number)] = packed(s.warning);
		palette[static_cast<size_t>(color::string)] = packed(s.success);
		palette[static_cast<size_t>(color::punctuation)] = packed(source.text);
		palette[static_cast<size_t>(color::preprocessor)] = packed(s.diff_removed);
		palette[static_cast<size_t>(color::identifier)] = packed(source.text);
		palette[static_cast<size_t>(color::knownIdentifier)] = packed(s.info);
		palette[static_cast<size_t>(color::comment)] = packed(s.text_secondary);
		palette[static_cast<size_t>(color::background)] = packed(source.background);
		palette[static_cast<size_t>(color::cursor)] = packed(source.text);
		palette[static_cast<size_t>(color::selection)] = packed(source.selection);
		palette[static_cast<size_t>(color::whitespace)] = packed(faint);
		palette[static_cast<size_t>(color::matchingBracketBackground)] = packed(faint);
		palette[static_cast<size_t>(color::matchingBracketActive)] = packed(source.text);
		palette[static_cast<size_t>(color::matchingBracketLevel1)] = packed(s.info);
		palette[static_cast<size_t>(color::matchingBracketLevel2)] = packed(s.success);
		palette[static_cast<size_t>(color::matchingBracketLevel3)] = packed(s.warning);
		palette[static_cast<size_t>(color::matchingBracketError)] = packed(s.error);
		palette[static_cast<size_t>(color::lineNumber)] = packed(dim);
		palette[static_cast<size_t>(color::currentLineNumber)] = packed(source.text);
		return palette;
	}
}
