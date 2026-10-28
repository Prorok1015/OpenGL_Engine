#include "edt_problems_panel.h"
#include <imgui.h>
#include <format>
#include <utility>

namespace {
	std::size_t index_of(edt::problem_severity severity)
	{
		return static_cast<std::size_t>(severity);
	}

	ImVec4 color_of(edt::problem_severity severity)
	{
		switch (severity) {
		case edt::problem_severity::error: return { 1.f, 0.3f, 0.3f, 1.f };
		case edt::problem_severity::warning: return { 1.f, 0.8f, 0.f, 1.f };
		case edt::problem_severity::note: return { 0.7f, 0.7f, 0.7f, 1.f };
		}
		return { 1.f, 1.f, 1.f, 1.f };
	}
}

std::string edt::problem_display_file(std::string_view file, const std::filesystem::path& root)
{
	if (file.empty() || root.empty()) return std::string(file);
	// Compiler output is UTF-8; a std::string would be read in the ANSI code page on Windows.
	const std::filesystem::path path(std::u8string_view(reinterpret_cast<const char8_t*>(file.data()), file.size()));
	if (!path.is_absolute()) return std::string(file);
	std::filesystem::path base = root.lexically_normal();
	if (!base.has_filename() && base.has_relative_path()) base = base.parent_path();
	const std::filesystem::path relative = path.lexically_normal().lexically_relative(base);
	if (relative.empty() || relative == "." || *relative.begin() == "..") return std::string(file);
	const std::u8string text = relative.generic_u8string();
	return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string edt::problem_copy_text(const problem& item)
{
	std::string out;
	if (!item.file.empty()) {
		out += item.file;
		if (item.line > 0) out += std::format("({})", item.line);
		out += ": ";
	}
	out += problem_severity_name(item.severity);
	if (!item.code.empty()) {
		out += ' ';
		out += item.code;
	}
	out += ": ";
	out += item.message;
	return out;
}

edt::problems_panel::problems_panel(const problem_list& problems, std::filesystem::path project_root)
	: panel_base("problems", "Problems")
	, m_problems(problems)
	, m_root(std::move(project_root))
{
}

void edt::problems_panel::refresh()
{
	if (m_generation == m_problems.generation()) return;
	m_generation = m_problems.generation();
	m_rows.clear();
	m_counts = {};
	for (auto& entry : m_problems.list()) {
		row added;
		added.location = problem_display_file(entry.item.file, m_root);
		if (entry.item.line > 0) added.location += std::format(":{}", entry.item.line);
		added.item = std::move(entry.item);
		++m_counts[index_of(added.item.severity)];
		m_rows.push_back(std::move(added));
	}
}

void edt::problems_panel::on_render()
{
	refresh();

	static constexpr const char* labels[] = { "Errors", "Warnings", "Notes" };
	for (std::size_t i = 0; i < m_shown.size(); ++i) {
		if (i > 0) ImGui::SameLine();
		const std::string label = std::format("{} ({})###problems_filter_{}", labels[i], m_counts[i], i);
		ImGui::Checkbox(label.c_str(), &m_shown[i]);
	}
	ImGui::Separator();

	if (m_rows.empty()) {
		ImGui::TextDisabled("No problems");
		return;
	}

	constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable
		| ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##problems_rows", 3, flags)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 70.f);
		ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 1.f);
		ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 3.f);
		ImGui::TableHeadersRow();
		for (std::size_t i = 0; i < m_rows.size(); ++i) {
			const row& entry = m_rows[i];
			const problem_severity severity = entry.item.severity;
			if (!m_shown[index_of(severity)]) continue;

			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushStyleColor(ImGuiCol_Text, color_of(severity));
			ImGui::Selectable(problem_severity_name(severity), false, ImGuiSelectableFlags_SpanAllColumns);
			ImGui::PopStyleColor();
			if (ImGui::BeginPopupContextItem("##problem_menu")) {
				if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(problem_copy_text(entry.item).c_str());
				ImGui::EndPopup();
			}

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(entry.location.c_str());

			ImGui::TableSetColumnIndex(2);
			if (!entry.item.code.empty()) {
				ImGui::TextDisabled("%s", entry.item.code.c_str());
				ImGui::SameLine();
			}
			ImGui::TextWrapped("%s", entry.item.message.c_str());
			if (!entry.item.notes.empty()) {
				ImGui::Indent();
				for (const auto& note : entry.item.notes) ImGui::TextDisabled("%s", note.c_str());
				ImGui::Unindent();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}
