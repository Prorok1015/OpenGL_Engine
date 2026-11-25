#include "edt_shader_text_view.h"
#include <imgui.h>

namespace edt
{
	namespace
	{
		ImVec4 severity_color(problem_severity severity, const gui::theme_semantic_colors& colors)
		{
			switch (severity) {
			case problem_severity::error: return colors.error;
			case problem_severity::warning: return colors.warning;
			case problem_severity::note: break;
			}
			return colors.info;
		}
	}

	shader_text_view::shader_text_view()
	{
		m_editor.SetLanguage(TextEditor::Language::Glsl());
		m_editor.SetTabSize(4);
	}

	void shader_text_view::load(const std::string& text)
	{
		m_editor.SetText(text);
		m_undo_index = m_editor.GetUndoIndex();
	}

	void shader_text_view::set_read_only(bool read_only)
	{
		m_read_only = read_only;
		m_editor.SetReadOnlyEnabled(read_only);
	}

	void shader_text_view::set_palette(const shader_palette_source& source)
	{
		m_editor.SetPalette(make_shader_palette(source));
	}

	void shader_text_view::set_markers(const shader_marker_set& markers, const gui::theme_semantic_colors& colors)
	{
		m_editor.ClearMarkers();
		for (const shader_line_marker& marker : markers.markers) {
			const ImU32 color = ImGui::ColorConvertFloat4ToU32(severity_color(marker.severity, colors));
			m_editor.AddMarker(marker.line - 1, color, color, marker.text, marker.text);
		}
	}

	void shader_text_view::insert_text(const std::string& text)
	{
		m_editor.ReplaceTextInCurrentCursor(text);
	}

	void shader_text_view::go_to_line(int line)
	{
		m_editor.SetCursor(line - 1, 0);
	}

	std::optional<std::string> shader_text_view::render(const char* id, const ImVec2& size)
	{
		m_editor.Render(id, size);
		const size_t undo_index = m_editor.GetUndoIndex();
		if (undo_index == m_undo_index) return std::nullopt;
		m_undo_index = undo_index;
		return m_editor.GetText();
	}
}
