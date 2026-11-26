#pragma once
#include "edt_shader_markers.h"
#include "edt_shader_palette.h"
#include <TextEditor.h>
#include <string>

namespace edt
{
	// The vendored text widget (#1202) behind the few calls the window needs. It holds no state of the document: the window gives it the
	// text, the markers and the palette, and gets the text back when the person changed it.
	class shader_text_view
	{
	public:
		shader_text_view();

		// Replaces the text and drops the widget's own undo history.
		void load(const std::string& text);
		void set_read_only(bool read_only);
		void set_palette(const shader_palette_source& source);
		// One marker per line of `markers` (1-based lines), coloured by severity; the old ones go.
		void set_markers(const shader_marker_set& markers, const gui::theme_semantic_colors& colors);
		// Types `text` at the cursor (over the selection), one step of the widget's undo.
		void insert_text(const std::string& text);
		// The cursor at the start of `line` (1-based), scrolled into view on the next render.
		void go_to_line(int line);

		// One frame of the widget. True when the person edited the text (typing, paste, undo, redo) since the last call. The text itself is
		// not copied here: `text()` does that, when the owner needs it.
		bool render(const char* id, const ImVec2& size);

		// The widget's own history; a step made here is not reported by the next `render` (the caller hands the text over itself).
		bool can_undo() const { return m_editor.CanUndo(); }
		bool can_redo() const { return m_editor.CanRedo(); }
		void undo();
		void redo();

		std::string text() const { return m_editor.GetText(); }
		int line_count() const { return m_editor.GetLineCount(); }
		bool has_markers() const { return m_editor.HasMarkers(); }
		bool read_only() const { return m_read_only; }

	private:
		TextEditor m_editor;
		size_t m_undo_index = 0;
		bool m_read_only = false;
	};
}
