#include "edt_asset_export_dialog.h"
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_system.h>
#include <snk/common/fs_paths.h>
#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Copies `text` into a fixed ImGui text buffer, truncating it, and always terminates it.
template<std::size_t N>
void copy_to_buffer(char (&dst)[N], std::string_view text)
{
	const std::size_t count = std::min(text.size(), N - 1);
	std::copy_n(text.data(), count, dst);
	dst[count] = '\0';
}

// What a failed export leaves in the project, for the person to keep or delete.
std::string written_before_text(const std::vector<res::tag>& written)
{
	if (written.empty()) return "\nNo file was written.";
	constexpr std::size_t shown = 8;
	std::string text = std::format("\n{} file(s) were written before and stay in the project:", written.size());
	for (std::size_t i = 0; i < written.size() && i < shown; ++i) {
		text += std::format("\n  {}", written[i].view());
	}
	if (written.size() > shown) text += std::format("\n  ... and {} more (see the log)", written.size() - shown);
	return text;
}

}

edt::asset_export_dialog::asset_export_dialog(asset_exporter& exporter, job_service& jobs, res::resource_system& resources)
	: m_exporter(exporter)
	, m_jobs(jobs)
	, m_resources(resources)
{
}

edt::asset_export_dialog::~asset_export_dialog() = default;

void edt::asset_export_dialog::open(import_result result)
{
	m_import = std::move(result);
	m_is_open = true;
	m_exported_tag = res::tag::null;
	m_exporting = false;
	m_export_error.clear();

	std::string name = m_import.source_path.stem().string();
	copy_to_buffer(m_asset_name, name);

	std::string folder = "assets/" + name;
	copy_to_buffer(m_target_folder, folder);

	m_browse_folder = false;
	m_needs_open = true;
	m_folder_dialog.set_current_path(m_resources.get_resources_path());
	m_folder_dialog.set_select_mode(file_dialog::SELECT_MODE::FOLDERS_ONLY);
	m_folder_dialog.clear_extension_filters();
}

bool edt::asset_export_dialog::render()
{
	if (!m_is_open)
		return false;

	if (m_needs_open) {
		ImGui::OpenPopup("Export to Project##edt_export");
		m_needs_open = false;
	}

	// Handle folder browser sub-dialog
	if (m_browse_folder) {
		bool folder_open = true;
		if (m_folder_dialog.show("Select Target Folder", &folder_open)) {
			auto selected = m_folder_dialog.get_selected_path();
			auto base = m_resources.get_resources_path();
			auto rel = selected.lexically_relative(base);
			// A folder outside res:// keeps its own path, so the form names the problem instead of exporting somewhere else.
			// The root of res:// itself is the empty folder.
			copy_to_buffer(m_target_folder, rel == "." ? std::string{} : fs_paths::generic_utf8_of(rel.empty() ? selected : rel));
			m_browse_folder = false;
			m_needs_open = true; // re-open the export popup (ImGui closed it while folder dialog was active)
		}
		if (!folder_open) {
			m_browse_folder = false;
			m_needs_open = true;
		}
		return true;
	}

	bool is_open = true;

	ImGui::SetNextWindowSize(ImVec2(500.f, 400.f), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal("Export to Project##edt_export", nullptr, ImGuiWindowFlags_None))
	{
		if (m_exporting)
			render_export_progress();
		else
			render_export_form();

		ImGui::EndPopup();
	} else {
		// Popup was closed externally (only if not exporting)
		if (!m_exporting) {
			m_is_open = false;
			is_open = false;
		}
	}

	return is_open;
}

void edt::asset_export_dialog::render_export_form()
{
	ImGui::Text("Asset name:");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputText("##asset_name", m_asset_name, sizeof(m_asset_name));

	ImGui::Spacing();

	ImGui::Text("Target folder (relative to res://):");
	float browse_w = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.f + ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetNextItemWidth(-browse_w);
	ImGui::InputText("##target_folder", m_target_folder, sizeof(m_target_folder));
	ImGui::SameLine();
	if (ImGui::Button("Browse...")) {
		m_browse_folder = true;
	}

	const std::string asset_name(m_asset_name);
	const std::string target_folder(m_target_folder);
	const ImVec4 error_color(1.0f, 0.4f, 0.4f, 1.0f);

	// build_remap asserts on a path holding "://", so the name and the folder are checked before it runs.
	std::string problem = asset_exporter::asset_name_problem(asset_name);
	if (problem.empty()) problem = asset_exporter::folder_problem(target_folder);
	std::unordered_map<res::tag, res::tag> remap;
	if (problem.empty()) {
		remap = m_exporter.build_remap(m_import, asset_name, target_folder);
		problem = asset_exporter::destination_problem(remap, asset_name, target_folder);
	}
	if (!problem.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, error_color);
		ImGui::TextWrapped("%s", problem.c_str());
		ImGui::PopStyleColor();
	} else if (target_folder.empty()) {
		ImGui::TextDisabled("Empty: the root of res://");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (problem.empty()) {
		ImGui::Text("Files to be created:");
		float buttons_height = ImGui::GetFrameHeightWithSpacing() * 2.f + ImGui::GetStyle().ItemSpacing.y * 3.f;
		if (!m_export_error.empty()) {
			const auto lines = 1 + std::ranges::count(m_export_error, '\n');
			buttons_height += ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(lines + 1);
		}
		float available = ImGui::GetContentRegionAvail().y - buttons_height;
		ImGui::BeginChild("##file_list", ImVec2(0.f, std::max(available, 80.f)),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		for (const auto& [mem_tag, res_tag] : remap) {
			const std::string rel(res_tag.relative());
			if (m_resources.exists(res_tag))
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s [overwrite]", rel.c_str());
			else
				ImGui::Text("%s", rel.c_str());
		}
		ImGui::EndChild();
	}

	if (!m_export_error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, error_color);
		ImGui::TextWrapped("Export failed: %s", m_export_error.c_str());
		ImGui::PopStyleColor();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const float bw = 110.f;
	const float avail = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX((avail - bw * 2.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

	const bool can_export = problem.empty();

	if (!can_export) ImGui::BeginDisabled();
	if (ImGui::Button("Export", { bw, 0.f })) {
		m_export_progress = std::make_shared<export_progress>();
		m_exporting = true;
		m_export_done = false;
		m_export_error.clear();

		// Capture by value for the background thread
		auto import_copy = m_import;
		auto progress_ptr = m_export_progress;
		auto* exporter_ptr = &m_exporter;
		auto written = std::make_shared<std::vector<res::tag>>();

		// io lane: the work is writing files to the project; the exporter reports through export_progress.
		// wait_for_writes: a write the resource system refuses ends the export with its reason instead of a "successful" one.
		m_export_job = m_jobs.start(ds::lane::io, { "Export " + asset_name, "export" }, m_lifetime,
			[exporter_ptr, import_copy = std::move(import_copy),
			 remap = std::move(remap), progress_ptr, written](ds::task_context& ctx) {
				ctx.throw_if_stopped();
				return exporter_ptr->export_to_project(import_copy, remap, progress_ptr.get(), &ctx, written.get(), true);
			},
			[this, written](ds::task_result<bool>& result) {
				if (result.status == ds::task_status::cancelled) {
					egLOG("edt/export", "Export cancelled");
				} else if (!result.ok()) {
					m_export_error = result.error.empty() ? std::string("the export stopped (see the log)") : result.error;
				} else if (!*result.value || !m_export_progress || !m_export_progress->success.load()) {
					m_export_error = m_export_progress && !m_export_progress->error.empty()
						? m_export_progress->error : std::string("a file could not be written (see the log)");
				} else if (!m_export_progress->skipped.empty()) {
					m_exported_tag = m_export_progress->exported_tag;
					std::string missing;
					for (const res::tag& tag : m_export_progress->skipped) missing += std::format("\n  {}", tag.view());
					egLOG_WARN("edt/export", "Exported {} without {} file(s) the model refers to but the import did not find:{}",
						m_exported_tag.view(), m_export_progress->skipped.size(), missing);
				} else {
					m_exported_tag = m_export_progress->exported_tag;
					egLOG("edt/export", "Export successful: {}", m_exported_tag.view());
				}
				if (!m_export_error.empty()) {
					// Not taken back: a written file may have replaced one the project had, which is gone by now.
					m_export_error += written_before_text(*written);
					egLOG_ERROR("edt/export", "Export failed: {}", m_export_error);
				}
				m_export_done = true;
			});
	}
	if (!can_export) ImGui::EndDisabled();

	ImGui::SameLine();

	if (ImGui::Button("Cancel", { bw, 0.f }) ||
		(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		 ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
		m_is_open = false;
		ImGui::CloseCurrentPopup();
	}
}

void edt::asset_export_dialog::render_export_progress()
{
	int done = m_export_progress->done.load();
	int total = m_export_progress->total.load();

	ImGui::Text("Exporting assets...");
	ImGui::Spacing();

	float fraction = (total > 0) ? static_cast<float>(done) / static_cast<float>(total) : 0.f;
	char overlay[64];
	snprintf(overlay, sizeof(overlay), "%d / %d", done, total);
	ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.f), overlay);

	ImGui::Spacing();

	// The job was cancelled and its result discarded: nothing more will arrive.
	if (!m_export_done && !m_jobs.is_running(m_export_job))
		m_export_done = true;

	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		m_jobs.cancel(m_export_job);
		m_export_done = true;
	}

	if (m_export_done) {
		m_exporting = false;
		m_export_progress.reset();
		// A failed export goes back to the form with the reason; the files it wrote before stay (see export_to_project).
		if (m_export_error.empty()) {
			m_is_open = false;
			ImGui::CloseCurrentPopup();
		}
	}
}
