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

	void shader_editor_panel::set_context(shader_source_document* shown)
	{
		if (m_ctx == shown) return;
		flush_pending(true);
		m_ctx = shown;
		m_error.clear();
		m_unplaced.clear();
		m_loaded = false;
		m_markers_ready = false;
		m_pending_line = 0;
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
		if (!is_open() && m_ctx && m_on_close) m_on_close();
	}

	void shader_editor_panel::on_render()
	{
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
		if (auto edited = m_view.render("##shader_text", ImGui::GetContentRegionAvail())) {
			m_pending_text = std::move(*edited);
			m_pending_since = ImGui::GetTime();
		}
		flush_pending(false);
	}

	void shader_editor_panel::flush_pending(bool force)
	{
		if (!m_pending_text) return;
		if (!force && ImGui::GetTime() - m_pending_since < k_typing_pause_seconds) return;
		std::string text = std::move(*m_pending_text);
		m_pending_text.reset();
		if (!m_ctx || !m_ctx->is_open()) return;
		if (m_ctx->set_text(std::move(text))) {
			m_synced_generation = m_ctx->generation();
		} else {
			m_loaded = false;
		}
	}

	void shader_editor_panel::render_toolbar()
	{
		const bool can_save = m_ctx->can_save() && !m_ctx->edits_locked();
		ImGui::BeginDisabled(!can_save || !m_ctx->is_dirty());
		if (ImGui::Button("Save") && m_on_save) {
			flush_pending(true);
			m_on_save();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!can_save || !m_ctx->is_dirty());
		if (ImGui::Button("Revert") && m_on_revert) {
			m_pending_text.reset();
			m_on_revert();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Close") && m_on_close) {
			flush_pending(true);
			m_on_close();
		}
		const std::string reason = m_ctx->read_only_reason();
		if (!reason.empty()) {
			ImGui::SameLine();
			ImGui::TextColored(colors().warning, "%s", reason.c_str());
		}
	}

	void shader_editor_panel::render_banners()
	{
		if (m_ctx->changed_on_disk()) {
			ImGui::TextColored(colors().warning, "The file changed on disk. Reload drops your edits, Overwrite replaces the file with them.");
			if (ImGui::Button("Reload")) {
				m_pending_text.reset();
				m_error = m_ctx->revert();
			}
			ImGui::SameLine();
			if (ImGui::Button("Overwrite")) {
				const save_result saved = m_ctx->overwrite();
				m_error = saved.saved ? std::string{} : saved.error;
			}
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
		m_view.load(m_ctx->text());
		m_synced_generation = m_ctx->generation();
		m_loaded = true;
		m_markers_ready = false;
	}

	void shader_editor_panel::sync_markers()
	{
		if (!m_problems) return;
		const std::uint64_t problems_generation = m_problems->generation();
		const std::uint64_t document_generation = m_ctx->generation();
		if (m_markers_ready && problems_generation == m_markers_problems_generation
			&& document_generation == m_markers_document_generation) return;
		const shader_marker_set markers = shader_markers_for(m_problems->list(), m_ctx->source().string(), m_view.line_count());
		m_view.set_markers(markers, colors());
		m_unplaced = markers.unplaced;
		m_markers_problems_generation = problems_generation;
		m_markers_document_generation = document_generation;
		m_markers_ready = true;
	}
}
