#include "edt_asset_export_dialog.h"
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <string_view>

namespace {

// Copies `text` into a fixed ImGui text buffer, truncating it, and always terminates it.
template<std::size_t N>
void copy_to_buffer(char (&dst)[N], std::string_view text)
{
	const std::size_t count = std::min(text.size(), N - 1);
	std::copy_n(text.data(), count, dst);
	dst[count] = '\0';
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
			copy_to_buffer(m_target_folder, rel.string());
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

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Preview files
	std::string asset_name(m_asset_name);
	std::string target_folder(m_target_folder);

	if (!asset_name.empty() && !target_folder.empty()) {
		auto remap = m_exporter.build_remap(m_import, asset_name, target_folder);

		ImGui::Text("Files to be created:");
		float buttons_height = ImGui::GetFrameHeightWithSpacing() * 2.f + ImGui::GetStyle().ItemSpacing.y * 3.f;
		float available = ImGui::GetContentRegionAvail().y - buttons_height;
		ImGui::BeginChild("##file_list", ImVec2(0.f, std::max(available, 80.f)),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		for (const auto& [mem_tag, res_tag] : remap) {
			std::string rel(res_tag.relative());
			bool exists = std::filesystem::exists(
				m_resources.get_resources_path() / rel);
			if (exists)
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s [overwrite]", rel.c_str());
			else
				ImGui::Text("%s", rel.c_str());
		}
		ImGui::EndChild();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const float bw = 110.f;
	const float avail = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX((avail - bw * 2.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

	std::string asset_name_s(m_asset_name);
	std::string target_folder_s(m_target_folder);
	bool can_export = !asset_name_s.empty() && !target_folder_s.empty();

	if (!can_export) ImGui::BeginDisabled();
	if (ImGui::Button("Export", { bw, 0.f })) {
		auto remap = m_exporter.build_remap(m_import, asset_name_s, target_folder_s);

		m_export_progress = std::make_shared<export_progress>();
		m_exporting = true;
		m_export_done = false;

		// Capture by value for the background thread
		auto import_copy = m_import;
		auto progress_ptr = m_export_progress;
		auto* exporter_ptr = &m_exporter;

		// io lane: the work is writing files to the project; the exporter reports through export_progress.
		m_export_job = m_jobs.start(ds::lane::io, { "Export " + asset_name_s, "export" }, m_lifetime,
			[exporter_ptr, import_copy = std::move(import_copy),
			 remap = std::move(remap), progress_ptr](ds::task_context& ctx) {
				ctx.throw_if_stopped();
				return exporter_ptr->export_to_project(import_copy, remap, progress_ptr.get(), &ctx);
			},
			[this](ds::task_result<bool>& result) {
				if (!result.ok()) {
					egLOG_ERROR("edt/export", "Export failed: {}", result.error);
				} else if (m_export_progress && m_export_progress->success.load()) {
					m_exported_tag = m_export_progress->exported_tag;
					egLOG("edt/export", "Export successful: {}", m_exported_tag.view());
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
		m_is_open = false;
		ImGui::CloseCurrentPopup();
	}
}
