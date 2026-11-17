#include "edt_migration_panel.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

	void wrapped(const std::string& text)
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(text.c_str());
		ImGui::PopTextWrapPos();
	}

	void coloured(const ImVec4& colour, const std::string& text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, colour);
		wrapped(text);
		ImGui::PopStyleColor();
	}

	void bullets(const std::vector<std::string>& lines)
	{
		for (const std::string& line : lines) {
			ImGui::Bullet();
			ImGui::SameLine();
			wrapped(line);
		}
	}

	// The height left above a row of buttons.
	float body_height()
	{
		return std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y, 60.0f);
	}
}

edt::migration_panel::migration_panel(editor_events& events, migration_window_model::ports ports)
	: panel_base(k_id, k_title), m_model(events, std::move(ports))
{
	m_open = false;
}

void edt::migration_panel::show_and_plan()
{
	m_open = true;
	m_focus = true;
	m_model.request_plan();
}

void edt::migration_panel::show_rollback_question(const std::string& id)
{
	m_open = true;
	m_focus = true;
	m_model.ask_roll_back(id);
}

void edt::migration_panel::on_before_begin()
{
	ImGui::SetNextWindowSize(ImVec2(em(720.f), em(520.f)), ImGuiCond_FirstUseEver);
	if (m_focus) {
		ImGui::SetNextWindowFocus();
		m_focus = false;
	}
}

void edt::migration_panel::on_render()
{
	// The scan blocks the frame it runs in: it runs at the start of the frame after the one that showed
	// "Reading the project...", so that line reaches the screen first.
	if (m_model.state() == migration_window_model::phase::planning && m_planning_drawn) {
		m_planning_drawn = false;
		m_model.tick();
	}
	switch (m_model.state()) {
	case migration_window_model::phase::idle: draw_idle(); break;
	case migration_window_model::phase::planning:
		ImGui::TextDisabled("Reading the project...");
		m_planning_drawn = true;
		break;
	case migration_window_model::phase::plan_shown: draw_plan(); break;
	case migration_window_model::phase::running: draw_running(); break;
	case migration_window_model::phase::report_shown: draw_report(); break;
	}
	draw_rollback_question();
}

void edt::migration_panel::draw_rollback_button()
{
	const std::string id = m_model.rollback_id();
	if (id.empty()) return;
	ImGui::SameLine();
	if (ImGui::Button("Roll Back...")) m_model.ask_roll_back(id);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ("Put the files back from .snk/migrations/" + id + "/backup").c_str());
}

void edt::migration_panel::draw_rollback_question()
{
	constexpr const char* popup = "Roll back migration?##edt_migration_rollback";
	if (m_model.take_question_request()) ImGui::OpenPopup(popup);
	if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	const std::optional<migration_window_model::rollback_question>& question = m_model.question();
	if (!question) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	wrapped("Roll back migration " + question->id + "?");
	if (!question->refused.empty()) {
		coloured(colors().error, "This rollback would be refused: " + question->refused);
	} else {
		// Every path of the journal, however many: a long list scrolls inside its own region.
		if (ImGui::BeginChild("##rollback_files", ImVec2(em(640.0f), em(280.0f)), ImGuiChildFlags_Borders,
				ImGuiWindowFlags_HorizontalScrollbar)) {
			ImGui::TextUnformatted(question->text.c_str(), question->text.c_str() + question->text.size());
		}
		ImGui::EndChild();
		ImGui::TextUnformatted("A file changed since the migration wrote it refuses the whole rollback; nothing is written then.");
		ImGui::TextUnformatted("Open documents are closed and the level is opened again from disk.");
	}
	ImGui::BeginDisabled(!m_model.can_confirm_roll_back());
	if (ImGui::Button("Roll Back")) {
		m_model.confirm_roll_back();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		m_model.cancel_roll_back();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void edt::migration_panel::draw_error()
{
	if (m_model.error_code().empty()) return;
	coloured(colors().error, m_model.error_code() + ": " + m_model.error_text());
	if (!m_model.error_hint().empty()) wrapped(m_model.error_hint());
}

void edt::migration_panel::draw_idle()
{
	ImGui::TextWrapped("Brings the data of the open project up to this engine. Every file is backed up under .snk/migrations first.");
	draw_error();
	if (ImGui::Button("Check Project")) m_model.request_plan();
}

void edt::migration_panel::draw_plan()
{
	const migration_window_model::plan_view& plan = *m_model.plan();
	if (ImGui::BeginChild("##plan", ImVec2(0.0f, body_height()), ImGuiChildFlags_Borders)) {
		std::vector<std::string> warnings;
		for (const migration_window_model::file_entry& file : plan.files) {
			for (const std::string& warning : file.warnings) warnings.push_back(file.path + ": " + warning);
		}
		if (!m_model.notice().empty()) {
			coloured(colors().warning, m_model.notice());
			ImGui::Separator();
		}
		if (!warnings.empty()) {
			coloured(colors().warning, "Warnings (data may be lost):");
			bullets(warnings);
			ImGui::Separator();
		}
		if (!plan.refused.empty()) {
			coloured(colors().error, "The migration would be refused: " + plan.refused);
			if (!plan.unfinished.empty()) {
				ImGui::TextUnformatted("Unfinished migrations (Roll Back... below, or editor --migrate-rollback <id>):");
				bullets(plan.unfinished);
			}
			ImGui::Separator();
		}
		if (!plan.manifest.empty()) wrapped(plan.manifest);
		if (plan.files.empty() && plan.project_steps.empty()) {
			ImGui::TextUnformatted(plan.code == migration_window_model::k_code_done ? "Nothing to migrate: the project is up to date."
				: "No file is migrated.");
		}
		if (!plan.files.empty()) {
			ImGui::Text("Files (%d):", static_cast<int>(plan.files.size()));
			for (std::size_t i = 0; i < plan.files.size(); ++i) {
				const migration_window_model::file_entry& file = plan.files[i];
				ImGui::PushID(static_cast<int>(i));
				// An id of its own and the path as plain text: a "##" in a file name is shown, not taken for an id.
				const float row_x = ImGui::GetCursorPosX();
				if (ImGui::Selectable("##file", false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					m_model.open_file(file.path);
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Double click: open the file");
				ImGui::SameLine(row_x);
				ImGui::TextUnformatted(file.path.c_str());
				ImGui::Indent();
				bullets(file.steps);
				ImGui::Unindent();
				ImGui::PopID();
			}
		}
		if (!plan.project_steps.empty()) {
			ImGui::Separator();
			ImGui::TextUnformatted("Project steps:");
			bullets(plan.project_steps);
		}
		if (!plan.skipped.empty()) {
			ImGui::Separator();
			ImGui::TextUnformatted("Not migrated:");
			bullets(plan.skipped);
		}
		draw_error();
	}
	ImGui::EndChild();

	ImGui::BeginDisabled(!m_model.can_migrate());
	if (ImGui::Button("Migrate")) m_model.migrate();
	ImGui::EndDisabled();
	if (!m_model.can_migrate() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", plan.code == migration_window_model::k_code_refused ? "The migration would be refused (see above)"
			: "Nothing to migrate");
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh")) m_model.request_plan();
	draw_rollback_button();
	ImGui::SameLine();
	if (ImGui::Button("Close")) {
		m_model.close();
		m_open = false;
	}
}

void edt::migration_panel::draw_running()
{
	ImGui::TextUnformatted(m_model.rolling_back() ? "Rolling the migration back..." : "Migrating the project...");
	ImGui::TextDisabled("Open documents are closed; the level opens again from disk when it ends.");
}

void edt::migration_panel::draw_report()
{
	const migration_window_model::report_view& report = *m_model.report();
	if (report.code >= 0) {
		const bool good = report.code == migration_window_model::k_code_done || (!report.rollback && report.code == migration_window_model::k_code_partial);
		const std::string outcome = report.rollback ? migration_window_model::rollback_outcome_text(report.code)
			: migration_window_model::outcome_text(report.code);
		std::string line = std::format("{} (code {}", outcome, report.code);
		if (!report.status.empty()) line += ", " + report.status;
		if (!report.id.empty()) line += ", .snk/migrations/" + report.id;
		line += ")";
		coloured(good ? colors().success : colors().error, line);
	}
	if (!report.error_code.empty()) {
		coloured(colors().error, report.error_code + ": " + report.error_text);
		if (const std::string hint = migration_window_model::hint_of(report.error_code); !hint.empty()) wrapped(hint);
	}
	if (!report.refused.empty()) coloured(colors().error, "Refused: " + report.refused);
	if (!report.level.empty()) wrapped(report.level);
	if (ImGui::BeginChild("##report", ImVec2(0.0f, body_height()), ImGuiChildFlags_Borders)) {
		if (!report.warnings.empty()) {
			coloured(colors().warning, "Warnings:");
			bullets(report.warnings);
			ImGui::Separator();
		}
		wrapped(report.text);
	}
	ImGui::EndChild();
	if (ImGui::Button("Close")) {
		m_model.close();
		m_open = false;
	}
	draw_rollback_button();
}
