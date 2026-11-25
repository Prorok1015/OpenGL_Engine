#include "edt_shader_editor_panel.h"
#include "edt_shader_source_document.h"
#include "edt_ui_metrics.h"
#include <imgui.h>
#include <cfloat>

namespace edt
{
	namespace
	{
		constexpr double k_typing_pause_seconds = 0.3;
	}

	shader_editor_panel::shader_editor_panel()
		: panel_base("shader_editor", "Shader")
	{
	}

	shader_editor_panel::~shader_editor_panel()
	{
		flush_pending(true);
		if (m_ctx) {
			m_ctx->set_edit_source({}, {});
			m_ctx->set_history_source({});
		}
	}

	void shader_editor_panel::set_context(shader_source_document* shown)
	{
		if (m_ctx == shown) return;
		flush_pending(true);
		if (m_ctx) {
			m_ctx->set_edit_source({}, {});
			m_ctx->set_history_source({});
		}
		m_ctx = shown;
		if (m_ctx) {
			m_ctx->set_edit_source([this] { return m_pending; }, [this] { flush_pending(true); }, [this] { return m_view.text(); });
			m_ctx->set_history_source({ [this] { return m_view.can_undo(); }, [this] { return m_view.can_redo(); },
				[this] { step_history(false); }, [this] { step_history(true); } });
		}
		m_error.clear();
		m_unplaced.clear();
		m_loaded = false;
		m_markers_ready = false;
		m_close_reported = false;
		m_pending = false;
		m_held_back = false;
		m_pending_line = 0;
	}

	bool shader_editor_panel::is_dirty() const
	{
		return m_ctx && m_ctx->is_open() && (m_ctx->is_dirty() || m_pending);
	}

	void shader_editor_panel::press_close()
	{
		flush_pending(true);
		if (m_on_close) m_on_close();
	}

	void shader_editor_panel::request_revert()
	{
		if (!m_ctx || !m_ctx->is_open()) return;
		m_error = m_ctx->revert();
		if (m_error.empty()) {
			m_pending = false;
			m_held_back = false;
		}
	}

	void shader_editor_panel::request_overwrite()
	{
		if (!m_ctx || !m_ctx->is_open()) return;
		flush_pending(true);
		const save_result saved = m_ctx->overwrite();
		m_error = saved.saved ? std::string{} : saved.error;
	}

	void shader_editor_panel::on_before_begin()
	{
		m_title = (m_ctx && m_ctx->is_open() ? "Shader: " + m_ctx->display_title() : std::string{ "Shader Editor" }) + "###shader_editor";
		ImGui::SetNextWindowSize(ImVec2(em(720.f), em(520.f)), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(em(320.f), em(200.f)), ImVec2(FLT_MAX, FLT_MAX));
	}

	void shader_editor_panel::on_hidden()
	{
		flush_pending(true);
		if (is_open()) {
			m_close_reported = false;
		} else if (m_ctx && m_on_close && !m_close_reported) {
			m_close_reported = true;
			m_on_close();
		}
	}

	void shader_editor_panel::on_render()
	{
		m_close_reported = false;
		if (!m_ctx || !m_ctx->is_open()) {
			ImGui::TextDisabled("(no shader open)");
			return;
		}
		render_toolbar();
		if (!m_ctx || !m_ctx->is_open()) return;
		render_banners();
		ImGui::Separator();

		sync_text();
		sync_markers();
		if (m_pending_line > 0) {
			m_view.go_to_line(m_pending_line);
			m_pending_line = 0;
		}
		if (m_view.render("##shader_text", ImGui::GetContentRegionAvail())) {
			note_typing();
		}
		flush_pending(false);
	}

	void shader_editor_panel::note_typing()
	{
		m_pending = true;
		m_pending_since = ImGui::GetTime();
		// The widget keeps a marker on its line while the lines move under it: after an edit they are not where the build put them. They
		// stay off until the next build brings new rows.
		m_view.set_markers(shader_marker_set{}, colors());
		m_markers_problems_generation = m_problems ? m_problems->generation() : 0;
		m_markers_ready = true;
	}

	void shader_editor_panel::step_history(bool redo)
	{
		if (!m_ctx) return;
		if (redo) m_view.redo();
		else m_view.undo();
		m_pending = false;
		m_ctx->set_text(m_view.text());
		m_synced_generation = m_ctx->generation();
	}

	void shader_editor_panel::flush_pending(bool force)
	{
		if (!m_pending) return;
		if (!m_ctx || !m_ctx->is_open()) {
			m_pending = false;
			return;
		}
		if (!force && ImGui::GetTime() - m_pending_since < k_typing_pause_seconds) return;
		if (m_ctx->read_only() || m_ctx->edits_locked()) {
			m_held_back = true;
			return;
		}
		m_held_back = false;
		m_pending = false;
		m_ctx->set_text(m_view.text());
		m_synced_generation = m_ctx->generation();
	}

	void shader_editor_panel::render_toolbar()
	{
		const bool can_save = m_ctx->can_save() && !m_ctx->edits_locked();
		ImGui::BeginDisabled(!can_save || !is_dirty());
		if (ImGui::Button("Save") && m_on_save) {
			flush_pending(true);
			m_on_save();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!can_save || !is_dirty());
		if (ImGui::Button("Revert")) {
			if (m_on_revert) {
				m_on_revert();
			} else {
				request_revert();
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Close")) press_close();
		const std::string reason = m_ctx->read_only_reason();
		if (!reason.empty()) {
			ImGui::SameLine();
			ImGui::TextColored(colors().warning, "%s", reason.c_str());
		}
	}

	void shader_editor_panel::render_banners()
	{
		if (m_ctx->missing_on_disk()) {
			ImGui::TextColored(colors().warning, "The file was deleted or renamed outside the window. Overwrite writes it again with your text.");
			if (ImGui::Button("Overwrite")) request_overwrite();
		} else if (m_ctx->changed_on_disk()) {
			ImGui::TextColored(colors().warning, "The shader was changed outside the window. Reload takes that text, Overwrite keeps yours.");
			if (ImGui::Button("Reload")) request_revert();
			ImGui::SameLine();
			if (ImGui::Button("Overwrite")) request_overwrite();
		}
		if (m_held_back) {
			ImGui::TextColored(colors().warning, "Locked: your typing is not applied until Play stops.");
		}
		if (m_ctx->mixed_endings()) {
			ImGui::TextColored(colors().info, "The file mixes line endings; Save writes the commonest one everywhere.");
		}
		for (const std::string& row : m_unplaced) {
			ImGui::TextColored(colors().error, "%s", row.c_str());
		}
		if (!m_error.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", m_error.c_str());
			ImGui::PopStyleColor();
		}
	}

	void shader_editor_panel::sync_text()
	{
		m_view.set_palette(shader_palette_source_of_style(colors()));
		m_view.set_read_only(m_ctx->read_only() || m_ctx->edits_locked());
		if (m_loaded && m_synced_generation == m_ctx->generation()) return;
		if (m_loaded && m_pending) {
			// The document changed while typing waited for its handover: the typing goes in on top, and the person chooses.
			m_pending = false;
			const std::string typed = m_view.text();
			if (m_ctx->set_text(typed)) {
				m_ctx->raise_conflict();
			} else if (m_ctx->text() != typed) {
				m_pending = true;
				m_held_back = true;
			}
			m_synced_generation = m_ctx->generation();
			return;
		}
		// A save or an undo of ours moves the generation without changing the text: loading it again would drop the widget's history.
		if (m_loaded && m_view.text() == m_ctx->text()) {
			m_synced_generation = m_ctx->generation();
			return;
		}
		m_view.load(m_ctx->text());
		m_synced_generation = m_ctx->generation();
		m_loaded = true;
		m_markers_ready = false;
	}

	void shader_editor_panel::sync_markers()
	{
		if (!m_problems) return;
		const std::uint64_t problems_generation = m_problems->generation();
		if (m_markers_ready && problems_generation == m_markers_problems_generation) return;
		const shader_marker_set markers = shader_markers_for(m_problems->list(), m_ctx->source().string(), m_view.line_count());
		m_view.set_markers(markers, colors());
		m_unplaced = markers.unplaced;
		m_markers_problems_generation = problems_generation;
		m_markers_ready = true;
	}
}
