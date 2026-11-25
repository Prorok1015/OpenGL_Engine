#pragma once
#include "edt_shader_markers.h"
#include "edt_shader_text_view.h"
#include <snk/editor/edt_panel_base.h>
#include <snk/editor/edt_problem_list.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	class shader_source_document;

	// The window of the GLSL editor (#1202): the toolbar, the banners about the file and the text. It shows one document at a time (the
	// active one); the component says which. The text widget keeps the undo of the typing; the document gets the text after every edit.
	class shader_editor_panel : public panel_base
	{
	public:
		shader_editor_panel();

		// The shown document; the one shown before stops drawing. Null: none.
		void set_context(shader_source_document* shown);
		shader_source_document* context() const { return m_ctx; }
		// The Problems list; the window takes the `shader` rows of its file. Must outlive the window or be reset.
		void set_problems(const problem_list* problems) { m_problems = problems; }
		void set_on_save(std::function<void()> cb) { m_on_save = std::move(cb); }
		void set_on_revert(std::function<void()> cb) { m_on_revert = std::move(cb); }
		void set_on_close(std::function<void()> cb) { m_on_close = std::move(cb); }
		void press_close()
		{
			flush_pending(true);
			if (m_on_close) m_on_close();
		}
		// Types `text` at the cursor of the shown document's text.
		void insert_text(const std::string& text) { m_view.insert_text(text); }
		// The cursor goes to `line` (1-based) of the shown document on the next frame.
		void go_to_line(int line) { m_pending_line = line; }
		const std::string& error() const { return m_error; }
		// Gives the document the text typed so far. The window hands its text over after a pause in the typing, not at every key; whoever
		// saves or closes the document calls this first.
		void flush_edits() { flush_pending(true); }
		bool has_pending_edits() const { return m_pending_text.has_value(); }
		// What the last frame marked in the text, and the rows of the file that name no line.
		bool has_markers() const { return m_view.has_markers(); }
		const std::vector<std::string>& unplaced() const { return m_unplaced; }

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
		std::uint64_t m_markers_document_generation = 0;
		bool m_markers_ready = false;
		int m_pending_line = 0;
		std::optional<std::string> m_pending_text;
		double m_pending_since = 0.0;
	};
}
