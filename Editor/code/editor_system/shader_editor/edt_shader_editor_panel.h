#pragma once
#include "edt_shader_markers.h"
#include "edt_shader_text_view.h"
#include <snk/editor/edt_panel_base.h>
#include <snk/editor/edt_problem_list.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	class shader_source_document;

	// The window of the GLSL editor (#1202): the toolbar, the banners about the file and the text. It shows one document at a time (the
	// active one); the component says which.
	//
	// The text widget keeps the undo of the typing and the document the text it was given. The window hands the typing over after a pause
	// (`flush_edits`), never per key, and the two never part: a change of the document that comes while typing waits for the handover is not
	// loaded over it, the typing goes in on top and the person gets the Reload / Overwrite choice (`raise_conflict`); typing that cannot be
	// handed over (Play) stays in the widget, read-only, and goes in when it can.
	// Ownership: the document must outlive the window, or `set_context(nullptr)` comes first (it, like the destructor, hands the typing over).
	class shader_editor_panel : public panel_base
	{
	public:
		shader_editor_panel();
		~shader_editor_panel() override;

		// The shown document; the one shown before stops drawing. Null: none.
		void set_context(shader_source_document* shown);
		shader_source_document* context() const { return m_ctx; }
		// The Problems list; the window takes the `shader` rows of its file. Must outlive the window or be reset.
		void set_problems(const problem_list* problems) { m_problems = problems; }
		void set_on_save(std::function<void()> cb) { m_on_save = std::move(cb); }
		// Revert asks first where the component wants it (it then calls `request_revert`); without one the button reverts at once.
		void set_on_revert(std::function<void()> cb) { m_on_revert = std::move(cb); }
		void set_on_close(std::function<void()> cb) { m_on_close = std::move(cb); }
		void press_close();
		// What the buttons do: Revert and Reload take the file's text (the typing not handed over goes with it, only when that worked);
		// Overwrite hands the typing over first.
		void request_revert();
		void request_overwrite();
		// Edit > Undo / Redo of the document: one step of the widget's history, handed over to the document at once.
		void step_history(bool redo);
		// Types `text` at the cursor of the shown document's text.
		void insert_text(const std::string& text) { m_view.insert_text(text); }
		// The cursor goes to `line` (1-based) of the shown document on the next frame.
		void go_to_line(int line) { m_pending_line = line; }

		// Gives the document the text typed so far. Whoever saves or closes the document calls this first (or asks `is_dirty`).
		void flush_edits() { flush_pending(true); }
		bool has_pending_edits() const { return m_pending; }
		// The document has unsaved changes, counting the typing not handed over yet: what the close and quit questions ask.
		bool is_dirty() const;
		// Typing is held back because the document cannot be edited now (Play).
		bool edits_held_back() const { return m_held_back; }

		const std::string& error() const { return m_error; }
		// What the last frame marked in the text, and the rows of the file that name no line.
		bool has_markers() const { return m_view.has_markers(); }
		const std::vector<std::string>& unplaced() const { return m_unplaced; }
		// The widget's text (a copy).
		std::string widget_text() const { return m_view.text(); }

	protected:
		void on_before_begin() override;
		void on_render() override;
		void on_hidden() override;

	private:
		void render_toolbar();
		void render_banners();
		void flush_pending(bool force);
		void sync_text();
		void sync_markers();
		void note_typing();

		shader_source_document* m_ctx = nullptr;
		shader_text_view m_view;
		const problem_list* m_problems = nullptr;
		std::function<void()> m_on_save;
		std::function<void()> m_on_revert;
		std::function<void()> m_on_close;
		std::string m_error;
		std::vector<std::string> m_unplaced;
		std::uint64_t m_synced_generation = 0;
		bool m_loaded = false;
		std::uint64_t m_markers_problems_generation = 0;
		bool m_markers_ready = false;
		bool m_close_reported = false;
		int m_pending_line = 0;
		bool m_pending = false;
		bool m_held_back = false;
		double m_pending_since = 0.0;
	};
}
